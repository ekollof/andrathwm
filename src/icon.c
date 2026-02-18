/* See LICENSE file for copyright and license details.
 *
 * Icon loading and rendering utilities
 * Generic icon loader supporting multiple formats and async loading
 */

#include <cairo/cairo.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <gio/gio.h>
#include <gtk/gtk.h>
#include <librsvg/rsvg.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "icon.h"

/* Undefine glib's MAX/MIN to avoid conflicts with our util.h */
#ifdef MAX
#undef MAX
#endif
#ifdef MIN
#undef MIN
#endif

#include "util.h"
#include "log.h"

/* Configuration from awm config.h */
extern const unsigned int iconcachesize;
extern const unsigned int iconcachemaxentries;

/* Icon cache - hash table with LRU eviction */
typedef struct CacheEntry {
	char              *key;
	int                size;
	cairo_surface_t   *surface;
	struct CacheEntry *next;     /* hash chain */
	struct CacheEntry *lru_prev; /* LRU list */
	struct CacheEntry *lru_next; /* LRU list */
} CacheEntry;

static CacheEntry **icon_cache  = NULL;
static CacheEntry  *lru_head    = NULL; /* most recently used */
static CacheEntry  *lru_tail    = NULL; /* least recently used */
static int          cache_count = 0;

/* Forward declarations for internal functions */
static cairo_surface_t *icon_load_svg(const char *path, int size);
static cairo_surface_t *icon_load_theme(const char *name, int size);
static unsigned int     hash_string(const char *str);
static cairo_surface_t *cache_get(const char *key, int size);
static void cache_put(const char *key, int size, cairo_surface_t *surface);
static void icon_loaded_callback(
    GObject *source_object, GAsyncResult *res, gpointer user_data);
static void file_read_callback(
    GObject *source_object, GAsyncResult *res, gpointer user_data);

/* LRU list helpers */
static void lru_remove(CacheEntry *entry);
static void lru_push_front(CacheEntry *entry);
static void cache_evict_lru(void);
static void cache_print_stats(void);

/* ============================================================================
 * Icon Cache
 * ============================================================================
 */

static unsigned int
hash_string(const char *str)
{
	unsigned int hash = 5381;
	int          c;

	while ((c = *str++))
		hash = ((hash << 5) + hash) + c;

	if (!iconcachesize)
		return 0;
	return hash % iconcachesize;
}

static void
lru_remove(CacheEntry *entry)
{
	if (entry->lru_prev)
		entry->lru_prev->lru_next = entry->lru_next;
	else
		lru_head = entry->lru_next;

	if (entry->lru_next)
		entry->lru_next->lru_prev = entry->lru_prev;
	else
		lru_tail = entry->lru_prev;

	entry->lru_prev = entry->lru_next = NULL;
}

static void
lru_push_front(CacheEntry *entry)
{
	entry->lru_next = lru_head;
	entry->lru_prev = NULL;

	if (lru_head)
		lru_head->lru_prev = entry;

	lru_head = entry;

	if (!lru_tail)
		lru_tail = entry;
}

static void
cache_evict_lru(void)
{
	CacheEntry  *entry = lru_tail;
	unsigned int hash;
	CacheEntry **pp;

	if (!entry)
		return;

	lru_remove(entry);

	hash = hash_string(entry->key);
	pp   = &icon_cache[hash];
	while (*pp) {
		if (*pp == entry) {
			*pp = entry->next;
			break;
		}
		pp = &(*pp)->next;
	}

	free(entry->key);
	cairo_surface_destroy(entry->surface);
	free(entry);
	cache_count--;
}

static void
cache_print_stats(void)
{
	awm_debug("icon cache: %d/%u entries", cache_count, iconcachemaxentries);
}

static void
cache_init(void)
{
	if (!iconcachesize)
		return;
	icon_cache = calloc(iconcachesize, sizeof(CacheEntry *));
}

static void
cache_cleanup(void)
{
	int         i;
	CacheEntry *entry, *next;

	if (!icon_cache)
		return;

	cache_print_stats();

	for (i = 0; i < iconcachesize; i++) {
		for (entry = icon_cache[i]; entry; entry = next) {
			next = entry->next;
			free(entry->key);
			if (entry->surface)
				cairo_surface_destroy(entry->surface);
			free(entry);
		}
		icon_cache[i] = NULL;
	}

	free(icon_cache);
	icon_cache  = NULL;
	lru_head    = NULL;
	lru_tail    = NULL;
	cache_count = 0;
}

static cairo_surface_t *
cache_get(const char *key, int size)
{
	unsigned int hash;
	CacheEntry  *entry;

	if (!key || !icon_cache || !iconcachesize)
		return NULL;

	hash = hash_string(key);

	for (entry = icon_cache[hash]; entry; entry = entry->next) {
		if (entry->size == size && strcmp(entry->key, key) == 0) {
			lru_remove(entry);
			lru_push_front(entry);
			cairo_surface_reference(entry->surface);
			return entry->surface;
		}
	}

	return NULL;
}

static void
cache_put(const char *key, int size, cairo_surface_t *surface)
{
	unsigned int hash;
	CacheEntry  *entry;

	if (!key || !surface || !icon_cache || !iconcachesize)
		return;

	hash = hash_string(key);

	for (entry = icon_cache[hash]; entry; entry = entry->next) {
		if (entry->size == size && strcmp(entry->key, key) == 0)
			return;
	}

	while (
	    iconcachemaxentries > 0 && cache_count >= (int) iconcachemaxentries) {
		cache_evict_lru();
	}

	entry = calloc(1, sizeof(CacheEntry));
	if (!entry)
		return;

	entry->key = strdup(key);
	if (!entry->key) {
		free(entry);
		return;
	}
	entry->size    = size;
	entry->surface = surface;
	cairo_surface_reference(surface);

	entry->next      = icon_cache[hash];
	icon_cache[hash] = entry;

	lru_push_front(entry);
	cache_count++;
}

/* ============================================================================
 * SVG Loading
 * ============================================================================
 */

static cairo_surface_t *
icon_load_svg(const char *path, int size)
{
	GError          *error = NULL;
	RsvgHandle      *handle;
	RsvgRectangle    viewport;
	cairo_surface_t *surface;
	cairo_t         *cr;

	handle = rsvg_handle_new_from_file(path, &error);
	if (!handle) {
		if (error) {
			awm_error("Failed to load SVG '%s': %s", path, error->message);
			g_error_free(error);
		}
		return NULL;
	}

	surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, size, size);
	cr      = cairo_create(surface);

	viewport.x      = 0;
	viewport.y      = 0;
	viewport.width  = size;
	viewport.height = size;

	if (!rsvg_handle_render_document(handle, cr, &viewport, &error)) {
		if (error) {
			awm_error("Failed to render SVG '%s': %s", path, error->message);
			g_error_free(error);
		}
		cairo_destroy(cr);
		cairo_surface_destroy(surface);
		g_object_unref(handle);
		return NULL;
	}

	cairo_destroy(cr);
	g_object_unref(handle);

	return surface;
}

/* ============================================================================
 * GdkPixbuf to Cairo Surface Conversion
 * ============================================================================
 */

cairo_surface_t *
icon_pixbuf_to_surface(GdkPixbuf *orig_pixbuf, int size)
{
	GdkPixbuf       *pixbuf, *scaled;
	cairo_surface_t *surface;
	cairo_t         *cr;
	int              width, height;

	if (!orig_pixbuf)
		return NULL;

	pixbuf = orig_pixbuf;
	width  = gdk_pixbuf_get_width(pixbuf);
	height = gdk_pixbuf_get_height(pixbuf);

	/* Scale to target size if needed */
	if (width != size || height != size) {
		scaled =
		    gdk_pixbuf_scale_simple(pixbuf, size, size, GDK_INTERP_BILINEAR);
		if (!scaled) {
			awm_error("Failed to scale icon to %dx%d", size, size);
			return NULL;
		}
		pixbuf = scaled;
		width  = size;
		height = size;
	} else {
		/* Keep reference if we're using the original */
		g_object_ref(pixbuf);
	}

	/* Create Cairo surface from pixbuf */
	surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
	cr      = cairo_create(surface);

	/* Draw pixbuf to surface - manual pixel copy since
	 * gdk_cairo_set_source_pixbuf may not be available */
	{
		unsigned char *pixels       = gdk_pixbuf_get_pixels(pixbuf);
		int            rowstride    = gdk_pixbuf_get_rowstride(pixbuf);
		int            n_channels   = gdk_pixbuf_get_n_channels(pixbuf);
		gboolean       has_alpha    = gdk_pixbuf_get_has_alpha(pixbuf);
		unsigned char *surface_data = cairo_image_surface_get_data(surface);
		int surface_stride          = cairo_image_surface_get_stride(surface);
		int x, y;

		cairo_surface_flush(surface);

		for (y = 0; y < height; y++) {
			for (x = 0; x < width; x++) {
				unsigned char  r, g, b, a;
				unsigned char *p = pixels + y * rowstride + x * n_channels;
				unsigned char *q = surface_data + y * surface_stride + x * 4;

				r = p[0];
				g = p[1];
				b = p[2];
				a = has_alpha ? p[3] : 255;

				/* Cairo uses pre-multiplied alpha in ARGB32 format */
				if (a == 0) {
					q[0] = q[1] = q[2] = q[3] = 0;
				} else if (a == 255) {
					q[0] = b;
					q[1] = g;
					q[2] = r;
					q[3] = a;
				} else {
					q[0] = (b * a) / 255;
					q[1] = (g * a) / 255;
					q[2] = (r * a) / 255;
					q[3] = a;
				}
			}
		}

		cairo_surface_mark_dirty(surface);
	}

	cairo_destroy(cr);
	g_object_unref(pixbuf);

	return surface;
}

/* ============================================================================
 * Icon Theme Loading
 * ============================================================================
 */

static cairo_surface_t *
icon_load_theme(const char *name, int size)
{
	cairo_surface_t *surface = NULL;
	GtkIconTheme    *icon_theme;
	GtkIconInfo     *icon_info;
	GdkPixbuf       *pixbuf;
	GError          *error = NULL;

	if (!name || name[0] == '\0')
		return NULL;

	/* Validate pointer looks reasonable (not obviously corrupt) */
	if (((unsigned long) name) < 4096) {
		awm_error("Invalid icon name pointer: %p", (void *) name);
		return NULL;
	}

	/* If name is an absolute path, try to load directly */
	if (name[0] == '/') {
		/* Check if file exists */
		if (access(name, R_OK) != 0)
			return NULL;

		/* Try SVG first (better quality at different sizes) */
		if (strstr(name, ".svg") || strstr(name, ".SVG"))
			return icon_load_svg(name, size);

		/* Use gdk-pixbuf for all other formats (.ico, .png, .jpg, .bmp, etc.)
		 */
		{
			GdkPixbuf *pixbuf;
			GError    *error = NULL;

			pixbuf = gdk_pixbuf_new_from_file(name, &error);
			if (!pixbuf) {
				if (error) {
					awm_error(
					    "Failed to load icon '%s': %s", name, error->message);
					g_error_free(error);
				}
				return NULL;
			}

			surface = icon_pixbuf_to_surface(pixbuf, size);
			g_object_unref(pixbuf);

			if (surface &&
			    cairo_surface_status(surface) == CAIRO_STATUS_SUCCESS)
				return surface;
			if (surface) {
				cairo_surface_destroy(surface);
				surface = NULL;
			}

			awm_error("Failed to load icon from file: %s", name);
			return NULL;
		}
	}

	/* Use GTK's icon theme to look up icon by name */
	icon_theme = gtk_icon_theme_get_default();

	icon_info = gtk_icon_theme_lookup_icon(icon_theme, name, size,
	    GTK_ICON_LOOKUP_USE_BUILTIN | GTK_ICON_LOOKUP_GENERIC_FALLBACK);

	if (!icon_info)
		return NULL;

	pixbuf = gtk_icon_info_load_icon(icon_info, &error);
	g_object_unref(icon_info);

	if (!pixbuf) {
		if (error) {
			awm_error("Failed to load icon '%s' from theme: %s", name,
			    error->message);
			g_error_free(error);
		}
		return NULL;
	}

	/* Convert GdkPixbuf to Cairo surface using our existing function */
	surface = icon_pixbuf_to_surface(pixbuf, size);
	g_object_unref(pixbuf);

	return surface;
}

/* ============================================================================
 * ARGB Pixmap to Cairo Surface Conversion
 * ============================================================================
 */

cairo_surface_t *
icon_pixmap_to_surface(Icon *icons, int count, int size)
{
	cairo_surface_t *surface;
	cairo_t         *cr;
	Icon            *best_icon = NULL;
	int              best_diff = INT_MAX;
	int              i;

	if (!icons || count == 0)
		return NULL;

	/* Find icon closest to requested size */
	for (i = 0; i < count; i++) {
		int diff = abs(icons[i].width - size);
		if (diff < best_diff) {
			best_diff = diff;
			best_icon = &icons[i];
		}
	}

	if (!best_icon || !best_icon->pixels)
		return NULL;

	/* Create Cairo surface from ARGB data */
	surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, size, size);
	if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
		cairo_surface_destroy(surface);
		return NULL;
	}

	cr = cairo_create(surface);

	/* Scale icon if necessary */
	if (best_icon->width != size || best_icon->height != size) {
		double scale_x = (double) size / best_icon->width;
		double scale_y = (double) size / best_icon->height;
		cairo_scale(cr, scale_x, scale_y);
	}

	/* Create source surface from pixmap data */
	cairo_surface_t *src = cairo_image_surface_create_for_data(
	    best_icon->pixels, CAIRO_FORMAT_ARGB32, best_icon->width,
	    best_icon->height,
	    cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, best_icon->width));

	/* Paint the icon */
	cairo_set_source_surface(cr, src, 0, 0);
	cairo_paint(cr);

	cairo_surface_destroy(src);
	cairo_destroy(cr);

	return surface;
}

/* ============================================================================
 * Async Icon Loading Callbacks
 * ============================================================================
 */

/* Callback after icon is loaded asynchronously */
static void
icon_loaded_callback(
    GObject *source_object, GAsyncResult *res, gpointer user_data)
{
	IconLoadData    *data      = (IconLoadData *) user_data;
	int              icon_size = data->size;
	GError          *error     = NULL;
	GdkPixbuf       *pixbuf;
	cairo_surface_t *icon_surface;

	if (!data->callback) {
		free(data);
		return;
	}

	/* Finish the async pixbuf load */
	pixbuf = gdk_pixbuf_new_from_stream_finish(res, &error);
	if (!pixbuf) {
		if (error)
			g_error_free(error);
		data->callback(NULL, data->user_data);
		free(data);
		return;
	}

	/* Convert pixbuf to cairo surface */
	icon_surface = icon_pixbuf_to_surface(pixbuf, icon_size);
	g_object_unref(pixbuf);

	/* Call user callback */
	data->callback(icon_surface, data->user_data);

	/* Cleanup - callback is responsible for destroying surface */
	free(data);
}

/* Callback when file is opened asynchronously */
static void
file_read_callback(
    GObject *source_object, GAsyncResult *res, gpointer user_data)
{
	IconLoadData     *data = (IconLoadData *) user_data;
	GFile            *file = G_FILE(source_object);
	GFileInputStream *stream;
	GError           *error = NULL;

	stream = g_file_read_finish(file, res, &error);
	g_object_unref(file);

	if (!stream) {
		if (error)
			g_error_free(error);
		if (data->callback)
			data->callback(NULL, data->user_data);
		free(data);
		return;
	}

	/* Start async pixbuf load - this returns immediately */
	gdk_pixbuf_new_from_stream_async(
	    G_INPUT_STREAM(stream), NULL, icon_loaded_callback, data);

	g_object_unref(stream);
}

/* ============================================================================
 * Public API
 * ============================================================================
 */

void
icon_init(void)
{
	/* Disable glycin loaders - they use subprocesses which can deadlock
	 * with async operations and window manager event loops */
	setenv("GDK_PIXBUF_DISABLE_GLYCIN", "1", 1);

	/* Initialize GTK for icon theme support (without requiring a main loop) */
	if (!gtk_init_check(NULL, NULL)) {
		awm_warn("Failed to initialize GTK, icon theme support may be limited");
	}

	/* Initialize icon cache */
	cache_init();
}

void
icon_cleanup(void)
{
	cache_cleanup();
}

cairo_surface_t *
icon_load(const char *name_or_path, int size)
{
	cairo_surface_t *surface;

	if (!name_or_path)
		return NULL;

	/* Check cache first */
	surface = cache_get(name_or_path, size);
	if (surface)
		return surface;

	/* Load icon */
	surface = icon_load_theme(name_or_path, size);
	if (!surface)
		return NULL;

	/* Cache it */
	cache_put(name_or_path, size, surface);

	return surface;
}

void
icon_load_async(const char *path, int size,
    void (*callback)(cairo_surface_t *surface, void *user_data),
    void *user_data)
{
	GFile        *file;
	IconLoadData *data;

	if (!path || !callback)
		return;

	data = malloc(sizeof(IconLoadData));
	if (!data)
		return;

	data->user_data = user_data;
	data->size      = size;
	data->callback  = callback;

	file = g_file_new_for_path(path);
	if (!file) {
		free(data);
		return;
	}

	/* Start async file read - this returns immediately */
	g_file_read_async(
	    file, G_PRIORITY_DEFAULT, NULL, file_read_callback, data);
}

void
icon_free(Icon *icon)
{
	if (icon) {
		free(icon->pixels);
		icon->pixels = NULL;
	}
}

void
icon_free_array(Icon *icons, int count)
{
	int i;

	if (!icons)
		return;

	for (i = 0; i < count; i++)
		icon_free(&icons[i]);

	free(icons);
}
