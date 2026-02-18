/* AndrathWM - Application Launcher
 * See LICENSE file for copyright and license details.
 *
 * rofi-style launcher that reads .desktop files and falls back to PATH
 */

#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "drw.h"
#include "icon.h"
#include "launcher.h"
#include "log.h"
#include "util.h"

#include <gtk/gtk.h>

#define LAUNCHER_INPUT_HEIGHT 28
#define LAUNCHER_ITEM_HEIGHT 24
#define LAUNCHER_PADDING 8
#define LAUNCHER_MIN_WIDTH 400
#define LAUNCHER_MAX_VISIBLE 12
#define LAUNCHER_SCROLL_BAR_WIDTH 6

static const char *desktop_paths[] = {
	"/usr/share/applications", "/usr/local/share/applications",
	NULL, /* will be replaced with ~/.local/share/applications */
	NULL, /* will be replaced with flatpak path */
};

static const char *skip_prefixes[] = {
	"gnome-",
	"kde-",
	"org.freedesktop.",
	"MIMEType",
	"Encoding",
};

/*
 * Build a one-time lookup table: lowercase(last dot component) -> full icon
 * name. Used to resolve icon names like "Alacritty" ->
 * "com.alacritty.Alacritty".
 */
typedef struct IconAlias {
	char             *short_lower; /* lowercase last component */
	char             *full_name;   /* actual theme icon name */
	struct IconAlias *next;
} IconAlias;

#define ICON_ALIAS_BUCKETS 256
static IconAlias *icon_alias_table[ICON_ALIAS_BUCKETS];
static int        icon_alias_built = 0;

static unsigned int
icon_alias_hash(const char *s)
{
	unsigned int h = 5381;
	int          c;
	while ((c = *s++))
		h = ((h << 5) + h) + (unsigned char) tolower(c);
	return h % ICON_ALIAS_BUCKETS;
}

static void
icon_alias_build(void)
{
	GtkIconTheme *theme;
	GList        *all, *l;

	if (icon_alias_built)
		return;
	icon_alias_built = 1;

	theme = gtk_icon_theme_get_default();
	if (!theme)
		return;

	all = gtk_icon_theme_list_icons(theme, NULL);
	for (l = all; l; l = l->next) {
		const char *name = (const char *) l->data;
		const char *dot  = strrchr(name, '.');
		/* only reverse-DNS style names (contain a dot) */
		if (!dot)
			continue;
		const char *last = dot + 1;
		char        lower[128];
		size_t      i;
		for (i = 0; last[i] && i < sizeof(lower) - 1; i++)
			lower[i] = (char) tolower((unsigned char) last[i]);
		lower[i] = '\0';

		unsigned int bucket = icon_alias_hash(lower);
		IconAlias   *a      = malloc(sizeof(IconAlias));
		if (!a)
			continue;
		a->short_lower           = strdup(lower);
		a->full_name             = strdup(name);
		a->next                  = icon_alias_table[bucket];
		icon_alias_table[bucket] = a;
	}
	g_list_free_full(all, g_free);
}

static void
icon_alias_free(void)
{
	int        i;
	IconAlias *a, *next;

	for (i = 0; i < ICON_ALIAS_BUCKETS; i++) {
		for (a = icon_alias_table[i]; a; a = next) {
			next = a->next;
			free(a->short_lower);
			free(a->full_name);
			free(a);
		}
		icon_alias_table[i] = NULL;
	}
	icon_alias_built = 0;
}

static const char *
icon_alias_lookup(const char *short_name)
{
	char         lower[128];
	size_t       i;
	unsigned int bucket;
	IconAlias   *a;

	for (i = 0; short_name[i] && i < sizeof(lower) - 1; i++)
		lower[i] = (char) tolower((unsigned char) short_name[i]);
	lower[i] = '\0';

	bucket = icon_alias_hash(lower);
	for (a = icon_alias_table[bucket]; a; a = a->next) {
		if (strcmp(a->short_lower, lower) == 0)
			return a->full_name;
	}
	return NULL;
}

static cairo_surface_t *
launcher_load_icon(const char *icon_name, int size)
{
	cairo_surface_t *surface;
	const char      *resolved;

	if (!icon_name || !*icon_name)
		return NULL;

	/* Try direct load first (handles absolute paths and exact theme names) */
	surface = icon_load(icon_name, size);
	if (surface)
		return surface;

	/*
	 * Fallback: resolve via alias table.
	 * Many apps use reverse-DNS icon names (e.g. com.alacritty.Alacritty)
	 * while the .desktop file just says Icon=Alacritty.
	 * Use icon_load() for the resolved name so SVG files go through
	 * icon_load_svg() rather than gdk_pixbuf_new_from_file(), which
	 * would bake in a white background.
	 */
	icon_alias_build();
	resolved = icon_alias_lookup(icon_name);
	if (!resolved)
		return NULL;

	return icon_load(resolved, size);
}

static int
launcher_item_matches(LauncherItem *item, const char *input)
{
	if (!input || !*input)
		return 1;

	char name_lower[256];
	char input_lower[256];
	strncpy(name_lower, item->name, sizeof(name_lower) - 1);
	name_lower[sizeof(name_lower) - 1] = '\0';
	strncpy(input_lower, input, sizeof(input_lower) - 1);
	input_lower[sizeof(input_lower) - 1] = '\0';

	char *p = name_lower;
	while (*p) {
		*p = tolower(*p);
		p++;
	}
	p = input_lower;
	while (*p) {
		*p = tolower(*p);
		p++;
	}

	return strstr(name_lower, input_lower) != NULL;
}

static int
launcher_is_executable(const char *path)
{
	struct stat st;
	return stat(path, &st) == 0 && S_ISREG(st.st_mode) && (st.st_mode & 0111);
}

static int
launcher_is_desktop_entry(const char *filename)
{
	size_t len = strlen(filename);
	return len > 8 && strcmp(filename + len - 8, ".desktop") == 0;
}

static char *
launcher_get_value(char *line, const char *key)
{
	size_t keylen = strlen(key);
	if (strncmp(line, key, keylen) == 0 && line[keylen] == '=') {
		char *val = line + keylen + 1;
		/* Strip trailing newline/carriage-return in place */
		size_t vlen = strlen(val);
		while (vlen > 0 && (val[vlen - 1] == '\n' || val[vlen - 1] == '\r'))
			val[--vlen] = '\0';
		return val;
	}
	return NULL;
}

static int
launcher_should_skip_entry(const char *name)
{
	size_t i;
	for (i = 0; i < sizeof(skip_prefixes) / sizeof(skip_prefixes[0]); i++) {
		if (strncmp(name, skip_prefixes[i], strlen(skip_prefixes[i])) == 0)
			return 1;
	}
	return 0;
}

static LauncherItem *
launcher_parse_desktop_file(const char *path)
{
	/* Check filename against skip_prefixes before opening the file */
	const char *basename = strrchr(path, '/');
	basename             = basename ? basename + 1 : path;

	/* Skip entries whose filename matches a known prefix */
	if (launcher_should_skip_entry(basename))
		return NULL;

	FILE         *fp;
	char         *line = NULL;
	size_t        len  = 0;
	ssize_t       read;
	char         *name       = NULL;
	char         *exec_cmd   = NULL;
	char         *icon       = NULL;
	int           no_display = 0;
	LauncherItem *item       = NULL;

	fp = fopen(path, "r");
	if (!fp)
		return NULL;

	while ((read = getline(&line, &len, fp)) != -1) {
		if (line[0] == '[') {
			if (name && exec_cmd && strcmp(line, "[Desktop Entry]\n") != 0)
				break;
			continue;
		}

		if (!name) {
			char *v = launcher_get_value(line, "Name");
			if (v)
				name = strdup(v);
		}
		if (!exec_cmd) {
			char *v = launcher_get_value(line, "Exec");
			if (v)
				exec_cmd = strdup(v);
		}
		if (!icon) {
			char *v = launcher_get_value(line, "Icon");
			if (v)
				icon = strdup(v);
		}
		char *v = launcher_get_value(line, "NoDisplay");
		if (v && strcmp(v, "true") == 0)
			no_display = 1;
		v = launcher_get_value(line, "Hidden");
		if (v && strcmp(v, "true") == 0)
			no_display = 1;
	}

	free(line);
	fclose(fp);

	if (!name || !exec_cmd || no_display) {
		if (name)
			free(name);
		if (exec_cmd)
			free(exec_cmd);
		if (icon)
			free(icon);
		return NULL;
	}

	item            = ecalloc(1, sizeof(LauncherItem));
	item->name      = name;
	item->exec      = exec_cmd;
	item->icon_name = icon;
	item->icon = icon ? launcher_load_icon(icon, LAUNCHER_ICON_SIZE) : NULL;
	if (icon && !item->icon)
		awm_debug("Launcher: failed to load icon '%s'", icon);
	item->is_desktop = 1;

	return item;
}

static LauncherItem *
launcher_load_desktop_files(const char *base_path)
{
	DIR           *dir;
	struct dirent *entry;
	LauncherItem  *items = NULL;
	LauncherItem  *last  = NULL;
	char           path[512];

	dir = opendir(base_path);
	if (!dir)
		return items;

	while ((entry = readdir(dir)) != NULL) {
		if (!launcher_is_desktop_entry(entry->d_name))
			continue;

		snprintf(path, sizeof(path), "%s/%s", base_path, entry->d_name);
		LauncherItem *item = launcher_parse_desktop_file(path);
		if (!item)
			continue;

		if (!items)
			items = item;
		else
			last->next = item;
		last = item;
	}

	closedir(dir);
	return items;
}

static LauncherItem *
launcher_find_duplicates(LauncherItem *items, const char *name)
{
	LauncherItem *item;
	for (item = items; item; item = item->next) {
		if (strcmp(item->name, name) == 0)
			return item;
	}
	return NULL;
}

static LauncherItem *
launcher_scan_path(LauncherItem *existing)
{
	DIR           *dir;
	struct dirent *entry;
	LauncherItem  *items = NULL;
	LauncherItem  *last  = NULL;
	char          *path_env;
	char          *path_copy;
	char          *dir_path;
	char           full_path[1024];

	path_env = getenv("PATH");
	if (!path_env)
		return items;

	path_copy = strdup(path_env);
	if (!path_copy)
		return items;

	dir_path = strtok(path_copy, ":");
	while (dir_path) {
		dir = opendir(dir_path);
		if (dir) {
			while ((entry = readdir(dir)) != NULL) {
				if (entry->d_name[0] == '.')
					continue;

				snprintf(full_path, sizeof(full_path), "%s/%s", dir_path,
				    entry->d_name);

				if (!launcher_is_executable(full_path))
					continue;

				/* Skip if already present in desktop items or this PATH list
				 */
				if (launcher_find_duplicates(existing, entry->d_name))
					continue;
				if (launcher_find_duplicates(items, entry->d_name))
					continue;

				LauncherItem *item = ecalloc(1, sizeof(LauncherItem));
				item->name         = strdup(entry->d_name);
				item->exec         = strdup(entry->d_name);
				item->is_desktop   = 0;

				if (!items)
					items = item;
				else
					last->next = item;
				last = item;
			}
			closedir(dir);
		}
		dir_path = strtok(NULL, ":");
	}

	free(path_copy);
	return items;
}

/*
 * History: persist launch counts in a plain-text file.
 * Format: one entry per line — "name\tcount\n".
 * Location: $XDG_STATE_HOME/awm/launcher_history
 *           (falls back to ~/.local/state/awm/launcher_history)
 */

#define LAUNCHER_HISTORY_TOP 10 /* items ranked above alphabetic block */

static void
launcher_history_path(char *out, size_t sz)
{
	const char *state = getenv("XDG_STATE_HOME");
	const char *home  = getenv("HOME");

	if (state && *state)
		snprintf(out, sz, "%s/awm/launcher_history", state);
	else if (home && *home)
		snprintf(out, sz, "%s/.local/state/awm/launcher_history", home);
	else
		snprintf(out, sz, "/tmp/awm_launcher_history");
}

static void
launcher_history_load(Launcher *launcher)
{
	FILE         *f;
	char          line[512];
	char         *tab;
	LauncherItem *item;

	f = fopen(launcher->history_path, "r");
	if (!f)
		return;

	while (fgets(line, sizeof(line), f)) {
		/* strip trailing newline */
		size_t len = strlen(line);
		while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
			line[--len] = '\0';

		tab = strchr(line, '\t');
		if (!tab)
			continue;
		*tab      = '\0';
		int count = atoi(tab + 1);
		if (count <= 0)
			continue;

		for (item = launcher->items; item; item = item->next) {
			if (strcmp(item->name, line) == 0) {
				item->launch_count = count;
				break;
			}
		}
	}
	fclose(f);
}

static void
launcher_history_save(Launcher *launcher)
{
	FILE         *f;
	LauncherItem *item;
	char          tmp[512];
	int           n;

	/* Ensure parent directory exists */
	n = snprintf(tmp, sizeof(tmp), "%s", launcher->history_path);
	/* strip filename component */
	while (n > 0 && tmp[n - 1] != '/')
		n--;
	if (n > 0) {
		tmp[n] = '\0';
		/* mkdir -p: create each component */
		for (int i = 1; i <= n; i++) {
			if (tmp[i] == '/' || tmp[i] == '\0') {
				char save = tmp[i];
				tmp[i]    = '\0';
				mkdir(tmp, 0700);
				tmp[i] = save;
			}
		}
	}

	f = fopen(launcher->history_path, "w");
	if (!f)
		return;

	for (item = launcher->items; item; item = item->next) {
		if (item->launch_count > 0)
			fprintf(f, "%s\t%d\n", item->name, item->launch_count);
	}
	fclose(f);
}

/*
 * Sort comparator for the filtered array.
 * Items with a positive launch_count are ranked by count descending
 * (up to LAUNCHER_HISTORY_TOP entries); everything else is alphabetic.
 */
static int
launcher_item_cmp(const void *a, const void *b)
{
	const LauncherItem *ia = *(const LauncherItem *const *) a;
	const LauncherItem *ib = *(const LauncherItem *const *) b;
	int                 ca = ia->launch_count > 0 ? ia->launch_count : 0;
	int                 cb = ib->launch_count > 0 ? ib->launch_count : 0;

	/* Both have history: sort by count descending */
	if (ca > 0 && cb > 0)
		return (cb > ca) - (ca > cb);

	/* Only one has history: it floats to the top */
	if (ca > 0)
		return -1;
	if (cb > 0)
		return 1;

	/* Neither has history: alphabetic by name */
	return strcasecmp(ia->name, ib->name);
}

static void
launcher_filter_items(Launcher *launcher)
{
	LauncherItem *item;
	int           count = 0;

	if (launcher->filtered) {
		free(launcher->filtered);
		launcher->filtered = NULL;
	}

	for (item = launcher->items; item; item = item->next) {
		if (launcher_item_matches(item, launcher->input))
			count++;
	}

	if (count == 0) {
		launcher->visible_count = 0;
		launcher->selected      = -1;
		return;
	}

	launcher->filtered = ecalloc(count + 1, sizeof(LauncherItem *));
	count              = 0;
	for (item = launcher->items; item; item = item->next) {
		if (launcher_item_matches(item, launcher->input)) {
			launcher->filtered[count++] = item;
		}
	}
	launcher->filtered[count] = NULL;
	launcher->visible_count   = count;
	launcher->scroll_offset   = 0;

	/* Sort: top LAUNCHER_HISTORY_TOP items by launch count float first,
	 * then everything (including lower-ranked history) is alphabetic. */
	qsort(
	    launcher->filtered, count, sizeof(LauncherItem *), launcher_item_cmp);

	launcher->selected = 0;
	if (launcher->selected >= launcher->visible_count)
		launcher->selected = launcher->visible_count - 1;
}

static void
launcher_calculate_size(Launcher *launcher)
{
	/* Use the pre-computed full-list maximum as the minimum width so the
	 * window never shrinks as the filter narrows the visible set. */
	unsigned int maxw = launcher->max_item_width > LAUNCHER_MIN_WIDTH
	    ? launcher->max_item_width
	    : LAUNCHER_MIN_WIDTH;

	launcher->w = maxw + LAUNCHER_PADDING * 2 + LAUNCHER_SCROLL_BAR_WIDTH;
	launcher->h = LAUNCHER_INPUT_HEIGHT + LAUNCHER_PADDING * 2;
	if (launcher->visible_count > LAUNCHER_MAX_VISIBLE)
		launcher->h += LAUNCHER_MAX_VISIBLE * LAUNCHER_ITEM_HEIGHT;
	else if (launcher->visible_count > 0)
		launcher->h += launcher->visible_count * LAUNCHER_ITEM_HEIGHT;
	else
		launcher->h += LAUNCHER_ITEM_HEIGHT;
}

static void
launcher_render(Launcher *launcher)
{
	int x, y;

	if (!launcher->drw || !launcher->scheme || !launcher->visible)
		return;

	/* Resize window if needed */
	if (launcher->drw->w < launcher->w || launcher->drw->h < launcher->h)
		drw_resize(launcher->drw, launcher->w, launcher->h);

	drw_setscheme(launcher->drw, launcher->scheme[0]);
	drw_rect(launcher->drw, 0, 0, launcher->w, launcher->h, 1, 0);

	drw_setscheme(launcher->drw, launcher->scheme[1]);
	drw_rect(launcher->drw, 0, 0, launcher->w,
	    LAUNCHER_INPUT_HEIGHT + LAUNCHER_PADDING * 2, 1, 0);

	x = LAUNCHER_PADDING;
	y = LAUNCHER_PADDING;

	char input_display[260];
	int  len = launcher->input_len;
	if (len > (int) sizeof(input_display) - 1)
		len = sizeof(input_display) - 1;
	memcpy(input_display, launcher->input, len);
	input_display[len] = '\0';

	drw_text(launcher->drw, x, y, launcher->w - LAUNCHER_PADDING * 2,
	    LAUNCHER_INPUT_HEIGHT, 0, input_display, 0);

	y += LAUNCHER_INPUT_HEIGHT + LAUNCHER_PADDING;

	if (launcher->visible_count == 0) {
		drw_setscheme(launcher->drw, launcher->scheme[0]);
		drw_text(launcher->drw, x, y, launcher->w - LAUNCHER_PADDING * 2,
		    LAUNCHER_ITEM_HEIGHT, 0, "(no matches)", 0);
		drw_map(launcher->drw, launcher->win, 0, 0, launcher->w, launcher->h);
		XSync(launcher->dpy, False);
		return;
	}

	int start_idx = launcher->scroll_offset;
	int end_idx   = start_idx + LAUNCHER_MAX_VISIBLE;
	if (end_idx > launcher->visible_count)
		end_idx = launcher->visible_count;

	for (int i = start_idx; i < end_idx; i++) {
		LauncherItem *item        = launcher->filtered[i];
		int           is_selected = (i == launcher->selected);

		if (is_selected)
			drw_setscheme(launcher->drw, launcher->scheme[1]);
		else
			drw_setscheme(launcher->drw, launcher->scheme[0]);

		drw_rect(launcher->drw, 0, y, launcher->w, LAUNCHER_ITEM_HEIGHT, 1, 1);

		/* Draw icon if available */
		if (item->icon) {
			int icon_x = x + 2;
			int icon_y = y + (LAUNCHER_ITEM_HEIGHT - LAUNCHER_ICON_SIZE) / 2;
			/* Paint background behind icon so alpha edges blend correctly */
			drw_rect(launcher->drw, icon_x, icon_y, LAUNCHER_ICON_SIZE,
			    LAUNCHER_ICON_SIZE, 1, 1);
			drw_pic(launcher->drw, icon_x, icon_y, LAUNCHER_ICON_SIZE,
			    LAUNCHER_ICON_SIZE, item->icon);
			drw_text(launcher->drw, x + LAUNCHER_ICON_SIZE + 6, y,
			    launcher->w - LAUNCHER_PADDING * 2 - LAUNCHER_ICON_SIZE - 4,
			    LAUNCHER_ITEM_HEIGHT, 0, item->name, 0);
		} else {
			drw_text(launcher->drw, x, y, launcher->w - LAUNCHER_PADDING * 2,
			    LAUNCHER_ITEM_HEIGHT, 0, item->name, 0);
		}

		y += LAUNCHER_ITEM_HEIGHT;
	}

	if (launcher->visible_count > LAUNCHER_MAX_VISIBLE) {
		unsigned int scroll_h = LAUNCHER_MAX_VISIBLE * LAUNCHER_ITEM_HEIGHT;
		unsigned int thumb_h =
		    (scroll_h * LAUNCHER_MAX_VISIBLE) / launcher->visible_count;
		unsigned int thumb_y =
		    (launcher->scroll_offset * scroll_h) / launcher->visible_count;

		drw_setscheme(launcher->drw, launcher->scheme[0]);
		drw_rect(launcher->drw, launcher->w - LAUNCHER_SCROLL_BAR_WIDTH - 2,
		    LAUNCHER_INPUT_HEIGHT + LAUNCHER_PADDING * 2, 2, scroll_h, 1, 0);

		drw_setscheme(launcher->drw, launcher->scheme[1]);
		drw_rect(launcher->drw, launcher->w - LAUNCHER_SCROLL_BAR_WIDTH - 2,
		    LAUNCHER_INPUT_HEIGHT + LAUNCHER_PADDING * 2 + thumb_y, 2, thumb_h,
		    1, 0);
	}

	drw_map(launcher->drw, launcher->win, 0, 0, launcher->w, launcher->h);
	XSync(launcher->dpy, False);
}

void
launcher_launch_selected(Launcher *launcher)
{
	if (!launcher || launcher->selected < 0 || !launcher->filtered)
		return;

	LauncherItem *item = launcher->filtered[launcher->selected];
	if (!item || !item->exec)
		return;

	/* Record the launch before hiding/forking */
	item->launch_count++;
	launcher_history_save(launcher);

	launcher_hide(launcher);

	if (fork() == 0) {
		if (launcher->dpy)
			close(ConnectionNumber(launcher->dpy));
		setsid();
		execlp("sh", "sh", "-c", item->exec, NULL);
		exit(1);
	}
}

static void
launcher_append_items(Launcher *launcher, LauncherItem *new_items)
{
	LauncherItem *last;

	if (!new_items)
		return;

	launcher->item_count = 0;
	for (LauncherItem *i = launcher->items; i; i = i->next)
		launcher->item_count++;

	for (LauncherItem *i = new_items; i; i = i->next)
		launcher->item_count++;

	last = launcher->items;
	if (!last) {
		launcher->items = new_items;
	} else {
		while (last->next)
			last = last->next;
		last->next = new_items;
	}
}

Launcher *
launcher_create(Display *dpy, Window root, Drw *drw, Clr **scheme)
{
	Launcher            *launcher;
	XSetWindowAttributes wa;
	char                *home;
	char                 path[512];
	int                  i;

	launcher            = ecalloc(1, sizeof(Launcher));
	launcher->dpy       = dpy;
	launcher->drw       = drw;
	launcher->scheme    = scheme;
	launcher->selected  = -1;
	launcher->w         = LAUNCHER_MIN_WIDTH;
	launcher->h         = 100;
	launcher->input[0]  = '\0';
	launcher->input_len = 0;

	/* Resolve history file path */
	launcher_history_path(
	    launcher->history_path, sizeof(launcher->history_path));

	home = getenv("HOME");
	if (!home)
		home = "/root";

	for (i = 0; desktop_paths[i] != NULL || i < 2; i++) {
		if (desktop_paths[i] == NULL) {
			if (i == 2) {
				snprintf(
				    path, sizeof(path), "%s/.local/share/applications", home);
			} else if (i == 3) {
				snprintf(path, sizeof(path),
				    "%s/.local/share/flatpak/exports/share/applications",
				    home);
			} else {
				continue;
			}
		} else {
			snprintf(path, sizeof(path), "%s", desktop_paths[i]);
		}

		LauncherItem *items = launcher_load_desktop_files(path);
		launcher_append_items(launcher, items);
	}

	LauncherItem *path_items = launcher_scan_path(launcher->items);
	launcher_append_items(launcher, path_items);

	/* Load launch history so counts are available before first filter/sort */
	launcher_history_load(launcher);

	/* Pre-compute the widest item width so the window never shrinks while
	 * typing — calculate over the full item list, not just visible items. */
	{
		LauncherItem *it;
		for (it = launcher->items; it; it = it->next) {
			unsigned int w = drw_fontset_getwidth(launcher->drw, it->name);
			if (it->icon)
				w += LAUNCHER_ICON_SIZE + 6;
			if (w > launcher->max_item_width)
				launcher->max_item_width = w;
		}
	}

	launcher_filter_items(launcher);
	launcher_calculate_size(launcher);

	wa.override_redirect = True;
	wa.background_pixel  = 0;
	wa.border_pixel      = 0;
	wa.colormap          = DefaultColormap(dpy, DefaultScreen(dpy));
	wa.event_mask        = ExposureMask | KeyPressMask | ButtonPressMask |
	    ButtonReleaseMask | PointerMotionMask;

	launcher->win = XCreateWindow(dpy, root, 0, 0, launcher->w, launcher->h, 1,
	    DefaultDepth(dpy, DefaultScreen(dpy)), CopyFromParent,
	    DefaultVisual(dpy, DefaultScreen(dpy)),
	    CWOverrideRedirect | CWBackPixel | CWBorderPixel | CWColormap |
	        CWEventMask,
	    &wa);

	return launcher;
}

void
launcher_free(Launcher *launcher)
{
	LauncherItem *item, *next;

	if (!launcher)
		return;

	if (launcher->visible)
		launcher_hide(launcher);

	if (launcher->win)
		XDestroyWindow(launcher->dpy, launcher->win);

	if (launcher->filtered)
		free(launcher->filtered);

	for (item = launcher->items; item; item = next) {
		next = item->next;
		free(item->name);
		free(item->exec);
		free(item->icon_name);
		if (item->icon)
			cairo_surface_destroy(item->icon);
		free(item);
	}

	free(launcher);
	icon_alias_free();
}

void
launcher_show(Launcher *launcher, int x, int y)
{
	if (!launcher)
		return;

	launcher->x          = x;
	launcher->y          = y;
	launcher->input[0]   = '\0';
	launcher->input_len  = 0;
	launcher->cursor_pos = 0;

	launcher_filter_items(launcher);
	launcher_calculate_size(launcher);

	int scr   = DefaultScreen(launcher->dpy);
	int mon_x = 0, mon_y = 0, mon_w = DisplayWidth(launcher->dpy, scr),
	    mon_h = DisplayHeight(launcher->dpy, scr);

	if (launcher->x + launcher->w > mon_x + mon_w)
		launcher->x = mon_x + mon_w - launcher->w;
	if (launcher->y + launcher->h > mon_y + mon_h)
		launcher->y = mon_y + mon_h - launcher->h;
	if (launcher->x < mon_x)
		launcher->x = mon_x;
	if (launcher->y < mon_y)
		launcher->y = mon_y;

	XMoveResizeWindow(launcher->dpy, launcher->win, launcher->x, launcher->y,
	    launcher->w, launcher->h);
	XMapRaised(launcher->dpy, launcher->win);
	XSync(launcher->dpy, False);

	launcher->visible = 1;
	launcher_render(launcher);

	XGrabPointer(launcher->dpy, launcher->win, False,
	    ButtonPressMask | ButtonReleaseMask | PointerMotionMask, GrabModeAsync,
	    GrabModeAsync, None, None, CurrentTime);
	XGrabKeyboard(launcher->dpy, launcher->win, True, GrabModeAsync,
	    GrabModeAsync, CurrentTime);
}

void
launcher_hide(Launcher *launcher)
{
	if (!launcher || !launcher->visible)
		return;

	XUngrabPointer(launcher->dpy, CurrentTime);
	XUngrabKeyboard(launcher->dpy, CurrentTime);
	XUnmapWindow(launcher->dpy, launcher->win);
	launcher->visible = 0;
}

static void
launcher_insert_text(Launcher *launcher, const char *s, int len)
{
	int avail;

	if (len <= 0)
		return;

	avail = (int) sizeof(launcher->input) - 1 - launcher->input_len;
	if (avail <= 0)
		return;
	if (len > avail)
		len = avail;

	if (launcher->cursor_pos < launcher->input_len) {
		memmove(launcher->input + launcher->cursor_pos + len,
		    launcher->input + launcher->cursor_pos,
		    launcher->input_len - launcher->cursor_pos);
	}
	memcpy(launcher->input + launcher->cursor_pos, s, len);
	launcher->input_len += len;
	launcher->cursor_pos += len;
	launcher->input[launcher->input_len] = '\0';

	launcher_filter_items(launcher);
	launcher_calculate_size(launcher);
	XResizeWindow(launcher->dpy, launcher->win, launcher->w, launcher->h);
	launcher_render(launcher);
}

/* Delete the character before the cursor (Backspace) */
static void
launcher_delete_char(Launcher *launcher)
{
	int pos, nbytes;

	if (launcher->cursor_pos <= 0 || launcher->input_len <= 0)
		return;

	/* Walk back over UTF-8 continuation bytes (10xxxxxx) to find the
	 * start of the codepoint, then delete the whole sequence. */
	pos = launcher->cursor_pos - 1;
	while (pos > 0 && ((unsigned char) launcher->input[pos] & 0xC0) == 0x80)
		pos--;
	nbytes = launcher->cursor_pos - pos;

	memmove(launcher->input + pos, launcher->input + launcher->cursor_pos,
	    launcher->input_len - launcher->cursor_pos);
	launcher->input_len -= nbytes;
	launcher->cursor_pos                 = pos;
	launcher->input[launcher->input_len] = '\0';

	launcher_filter_items(launcher);
	launcher_calculate_size(launcher);
	XResizeWindow(launcher->dpy, launcher->win, launcher->w, launcher->h);
	launcher_render(launcher);
}

/* Delete the character at the cursor (Delete) */
static void
launcher_delete_char_forward(Launcher *launcher)
{
	int end, nbytes;

	if (launcher->cursor_pos >= launcher->input_len)
		return;

	/* Determine the byte length of the codepoint at cursor.
	 * UTF-8 lead byte tells us how many bytes follow. */
	end = launcher->cursor_pos + 1;
	while (end < launcher->input_len &&
	    ((unsigned char) launcher->input[end] & 0xC0) == 0x80)
		end++;
	nbytes = end - launcher->cursor_pos;

	memmove(launcher->input + launcher->cursor_pos, launcher->input + end,
	    launcher->input_len - end);
	launcher->input_len -= nbytes;
	launcher->input[launcher->input_len] = '\0';

	launcher_filter_items(launcher);
	launcher_calculate_size(launcher);
	XResizeWindow(launcher->dpy, launcher->win, launcher->w, launcher->h);
	launcher_render(launcher);
}

/* Delete the word before the cursor (Ctrl+w) */
static void
launcher_delete_word(Launcher *launcher)
{
	int pos = launcher->cursor_pos;

	if (pos <= 0)
		return;

	/* Skip trailing spaces */
	while (pos > 0 && launcher->input[pos - 1] == ' ')
		pos--;
	/* Skip the word */
	while (pos > 0 && launcher->input[pos - 1] != ' ')
		pos--;

	int deleted = launcher->cursor_pos - pos;
	memmove(launcher->input + pos, launcher->input + launcher->cursor_pos,
	    launcher->input_len - launcher->cursor_pos);
	launcher->input_len -= deleted;
	launcher->cursor_pos                 = pos;
	launcher->input[launcher->input_len] = '\0';

	launcher_filter_items(launcher);
	launcher_calculate_size(launcher);
	XResizeWindow(launcher->dpy, launcher->win, launcher->w, launcher->h);
	launcher_render(launcher);
}

static void
launcher_move_cursor(Launcher *launcher, int delta)
{
	launcher->cursor_pos += delta;
	if (launcher->cursor_pos < 0)
		launcher->cursor_pos = 0;
	if (launcher->cursor_pos > launcher->input_len)
		launcher->cursor_pos = launcher->input_len;
}

static void
launcher_scroll(Launcher *launcher, int delta)
{
	int new_offset = launcher->scroll_offset + delta;

	if (new_offset < 0)
		new_offset = 0;
	if (new_offset + LAUNCHER_MAX_VISIBLE > launcher->visible_count)
		new_offset = launcher->visible_count - LAUNCHER_MAX_VISIBLE;
	if (new_offset < 0)
		new_offset = 0;

	launcher->scroll_offset = new_offset;

	if (launcher->selected < launcher->scroll_offset)
		launcher->selected = launcher->scroll_offset;
	if (launcher->selected >= launcher->scroll_offset + LAUNCHER_MAX_VISIBLE)
		launcher->selected =
		    launcher->scroll_offset + LAUNCHER_MAX_VISIBLE - 1;
}

int
launcher_handle_event(Launcher *launcher, XEvent *ev)
{
	KeySym key;
	int    idx, y;

	if (!launcher || !launcher->visible)
		return 0;

	if (ev->type == Expose) {
		if (ev->xexpose.count == 0)
			launcher_render(launcher);
		return 1;
	}

	if (ev->type == KeyPress) {
		key = XLookupKeysym(&ev->xkey, 0);

		switch (key) {
		case XK_Escape:
			launcher_hide(launcher);
			return 1;
		case XK_Return:
		case XK_KP_Enter:
			launcher_launch_selected(launcher);
			return 1;
		case XK_Up:
			if (launcher->selected > 0) {
				launcher->selected--;
				if (launcher->selected < launcher->scroll_offset)
					launcher_scroll(launcher, -1);
			}
			launcher_render(launcher);
			return 1;
		case XK_Down:
			if (launcher->selected < launcher->visible_count - 1) {
				launcher->selected++;
				if (launcher->selected >=
				    launcher->scroll_offset + LAUNCHER_MAX_VISIBLE)
					launcher_scroll(launcher, 1);
			}
			launcher_render(launcher);
			return 1;
		case XK_Page_Up:
			launcher_scroll(launcher, -LAUNCHER_MAX_VISIBLE);
			launcher_render(launcher);
			return 1;
		case XK_Page_Down:
			launcher_scroll(launcher, LAUNCHER_MAX_VISIBLE);
			launcher_render(launcher);
			return 1;
		case XK_Home:
			launcher->selected      = 0;
			launcher->scroll_offset = 0;
			launcher_render(launcher);
			return 1;
		case XK_End:
			launcher->selected = launcher->visible_count - 1;
			launcher->scroll_offset =
			    launcher->visible_count - LAUNCHER_MAX_VISIBLE;
			if (launcher->scroll_offset < 0)
				launcher->scroll_offset = 0;
			launcher_render(launcher);
			return 1;
		case XK_BackSpace:
			if (launcher->cursor_pos > 0) {
				launcher_delete_char(launcher);
			}
			return 1;
		case XK_Delete:
			if (launcher->cursor_pos < launcher->input_len) {
				launcher_delete_char_forward(launcher);
			}
			return 1;
		case XK_Left:
			launcher_move_cursor(launcher, -1);
			launcher_render(launcher);
			return 1;
		case XK_Right:
			launcher_move_cursor(launcher, +1);
			launcher_render(launcher);
			return 1;
		case XK_Tab:
			if (ev->xkey.state & ShiftMask) {
				if (launcher->selected > 0) {
					launcher->selected--;
					if (launcher->selected < launcher->scroll_offset)
						launcher_scroll(launcher, -1);
				}
			} else {
				if (launcher->selected < launcher->visible_count - 1) {
					launcher->selected++;
					if (launcher->selected >=
					    launcher->scroll_offset + LAUNCHER_MAX_VISIBLE)
						launcher_scroll(launcher, 1);
				}
			}
			launcher_render(launcher);
			return 1;
		default:
			break;
		}

		if (ev->xkey.state & ControlMask) {
			switch (key) {
			case XK_u:
				launcher->input[0]   = '\0';
				launcher->input_len  = 0;
				launcher->cursor_pos = 0;
				launcher_filter_items(launcher);
				launcher_calculate_size(launcher);
				launcher_render(launcher);
				return 1;
			case XK_k:
				launcher->input[launcher->cursor_pos] = '\0';
				launcher->input_len                   = launcher->cursor_pos;
				launcher_filter_items(launcher);
				launcher_calculate_size(launcher);
				launcher_render(launcher);
				return 1;
			case XK_w:
				launcher_delete_word(launcher);
				return 1;
			case XK_a:
				launcher->cursor_pos = 0;
				launcher_render(launcher);
				return 1;
			case XK_e:
				launcher->cursor_pos = launcher->input_len;
				launcher_render(launcher);
				return 1;
			}
		}

		/* Handle character input */
		{
			XComposeStatus status;
			char           buf[32];
			int            len;
			KeySym         ks;

			len = XLookupString(&ev->xkey, buf, sizeof(buf), &ks, &status);
			if (len > 0 &&
			    (isprint((unsigned char) buf[0]) ||
			        (unsigned char) buf[0] >= 0x80)) {
				launcher_insert_text(launcher, buf, len);
			}
		}

		return 1;
	}

	if (ev->type == MotionNotify) {
		if (ev->xmotion.y <
		    (int) (LAUNCHER_INPUT_HEIGHT + LAUNCHER_PADDING * 2)) {
			launcher->selected = -1;
			launcher_render(launcher);
			return 1;
		}

		y   = LAUNCHER_INPUT_HEIGHT + LAUNCHER_PADDING * 2;
		idx = launcher->scroll_offset;

		while (idx < launcher->visible_count &&
		    y + LAUNCHER_ITEM_HEIGHT <= (int) launcher->h) {
			if (ev->xmotion.y >= y &&
			    ev->xmotion.y < y + LAUNCHER_ITEM_HEIGHT) {
				if (launcher->selected != idx) {
					launcher->selected = idx;
					launcher_render(launcher);
				}
				return 1;
			}
			y += LAUNCHER_ITEM_HEIGHT;
			idx++;
		}
		return 1;
	}

	if (ev->type == ButtonPress || ev->type == ButtonRelease) {
		/* Swallow all wheel events — Button4/5 generate both press and
		 * release; if we only handle press, the release falls through to
		 * the launch path. */
		if (ev->xbutton.button == Button4 || ev->xbutton.button == Button5) {
			if (ev->type == ButtonPress) {
				if (ev->xbutton.button == Button4) {
					if (launcher->selected > 0)
						launcher->selected--;
					if (launcher->selected < launcher->scroll_offset)
						launcher_scroll(launcher, -1);
				} else {
					if (launcher->selected < launcher->visible_count - 1)
						launcher->selected++;
					if (launcher->selected >=
					    launcher->scroll_offset + LAUNCHER_MAX_VISIBLE)
						launcher_scroll(launcher, 1);
				}
				launcher_render(launcher);
			}
			return 1;
		}

		if (ev->xbutton.y <
		    (int) (LAUNCHER_INPUT_HEIGHT + LAUNCHER_PADDING * 2)) {
			launcher_hide(launcher);
			return 1;
		}

		if (ev->type == ButtonRelease && launcher->selected >= 0) {
			launcher_launch_selected(launcher);
			return 1;
		}
		return 1;
	}

	return 0;
}
