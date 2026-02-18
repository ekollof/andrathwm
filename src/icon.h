/* See LICENSE file for copyright and license details.
 *
 * Icon loading and rendering utilities
 * Generic icon loader supporting multiple formats and async loading
 */

#ifndef ICON_H
#define ICON_H

#include <cairo/cairo.h>
#include <gdk-pixbuf/gdk-pixbuf.h>

/* Icon structure for ARGB pixmap data */
typedef struct {
	int            width;
	int            height;
	unsigned char *pixels; /* ARGB32 format */
} Icon;

/* Callback data for async icon loading */
typedef struct {
	void *user_data;
	int   size;
	void (*callback)(cairo_surface_t *surface, void *user_data);
} IconLoadData;

/* Initialization - call before using icon functions */
void icon_init(void);

/* Cleanup - call on shutdown */
void icon_cleanup(void);

/* Synchronous icon loading from theme or file path
 * Returns cairo surface or NULL on failure
 * Supports: PNG, JPEG, SVG, ICO, BMP via theme name or absolute path
 * Caller must call cairo_surface_destroy() on returned surface
 */
cairo_surface_t *icon_load(const char *name_or_path, int size);

/* Asynchronous icon loading from file path
 * Calls callback(surface, user_data) when complete
 * Returns immediately, performs I/O and decoding in background
 */
void icon_load_async(const char *path, int size,
    void (*callback)(cairo_surface_t *surface, void *user_data),
    void *user_data);

/* Convert ARGB pixmap data to cairo surface
 * icons: array of Icon structs (different sizes)
 * count: number of icons in array
 * size: desired output size (best match selected)
 * Returns cairo surface or NULL on failure
 */
cairo_surface_t *icon_pixmap_to_surface(Icon *icons, int count, int size);

/* Convert GdkPixbuf to cairo surface with scaling
 * Returns cairo surface or NULL on failure
 */
cairo_surface_t *icon_pixbuf_to_surface(GdkPixbuf *pixbuf, int size);

/* Free icon pixmap data */
void icon_free(Icon *icon);
void icon_free_array(Icon *icons, int count);

#endif /* ICON_H */
