/* See LICENSE file for copyright and license details.
 *
 * drw_cairo.c — pure Cairo/PangoCairo drawing backend.
 *
 * Drop-in replacement for drw.c.  Implements every drw_* function with
 * identical signatures so no caller needs to change.  Build with:
 *
 *   make DRW_CAIRO=1
 *
 * Differences from drw.c:
 *   - drw_rect:        cairo_fill / cairo_stroke  (no xcb_poly_fill_rectangle)
 *   - drw_text:        Cairo background fill in the same cairo_t as text draw
 *                      (no xcb_change_gc, no
 * cairo_surface_mark_dirty_rectangle)
 *   - drw_pic:         cairo_set_source_surface + cairo_paint
 *                      (no xcb_put_image, no XRender picture machinery)
 *   - drw_clr_create:  hex parse only, pixel field set to 0
 *                      (no xcb_alloc_color round-trip)
 *   - GC in Drw:       minimal passthrough — created with 0 properties,
 *                      used only as the required GC argument to xcb_copy_area
 */

#include <stdlib.h>
#include <string.h>
#include <xcb/xcb.h>
#include <xcb/xproto.h>
#include <xcb/xcb_cursor.h>
#include <pango/pangocairo.h>

#include "drw.h"
#include "log.h"
#include "util.h"

/* ── internal helpers (identical to drw.c) ─────────────────────────────── */

static xcb_visualtype_t *
xcb_find_visualtype(xcb_connection_t *conn, int screen_num, xcb_visualid_t vid)
{
	const xcb_setup_t    *setup = xcb_get_setup(conn);
	xcb_screen_iterator_t si    = xcb_setup_roots_iterator(setup);

	for (int i = 0; i < screen_num; i++)
		xcb_screen_next(&si);

	xcb_depth_iterator_t di = xcb_screen_allowed_depths_iterator(si.data);
	for (; di.rem; xcb_depth_next(&di)) {
		xcb_visualtype_iterator_t vi = xcb_depth_visuals_iterator(di.data);
		for (; vi.rem; xcb_visualtype_next(&vi)) {
			if (vi.data->visual_id == vid)
				return vi.data;
		}
	}
	return NULL;
}

static uint8_t
drw_root_depth(xcb_connection_t *conn, int scr_num)
{
	xcb_screen_iterator_t it = xcb_setup_roots_iterator(xcb_get_setup(conn));
	for (int i = 0; i < scr_num; i++)
		xcb_screen_next(&it);
	return it.data->root_depth;
}

/* ── lifecycle ──────────────────────────────────────────────────────────── */

Drw *
drw_create(xcb_connection_t *xc, int screen, xcb_window_t root, unsigned int w,
    unsigned int h)
{
	Drw *drw = ecalloc(1, sizeof(Drw));

	drw->xc     = xc;
	drw->screen = screen;
	drw->root   = root;
	drw->w      = w;
	drw->h      = h;

	/* Backing pixmap */
	drw->drawable = xcb_generate_id(xc);
	xcb_create_pixmap(xc, drw_root_depth(xc, screen), drw->drawable, root,
	    (uint16_t) w, (uint16_t) h);

	/* Minimal GC — no drawing properties set; used only as the required
	 * GC argument to xcb_copy_area in drw_map. */
	drw->gc = xcb_generate_id(xc);
	xcb_create_gc(xc, drw->gc, drw->drawable, 0, NULL);

	/* Cairo XCB surface on the backing pixmap */
	{
		xcb_screen_iterator_t sit =
		    xcb_setup_roots_iterator(xcb_get_setup(xc));
		for (int i = 0; i < screen; i++)
			xcb_screen_next(&sit);
		drw->xcb_visual =
		    xcb_find_visualtype(xc, screen, sit.data->root_visual);
	}

	if (drw->xcb_visual) {
		drw->cairo_surface = cairo_xcb_surface_create(xc,
		    (xcb_drawable_t) drw->drawable, drw->xcb_visual, (int) w, (int) h);
		if (drw->cairo_surface &&
		    cairo_surface_status(drw->cairo_surface) != CAIRO_STATUS_SUCCESS) {
			cairo_surface_destroy(drw->cairo_surface);
			drw->cairo_surface = NULL;
		}
	}

	return drw;
}

void
drw_resize(Drw *drw, unsigned int w, unsigned int h)
{
	if (!drw)
		return;

	drw->w = w;
	drw->h = h;

	if (drw->drawable) {
		xcb_free_pixmap(drw->xc, drw->drawable);
		drw->drawable = xcb_generate_id(drw->xc);
		xcb_create_pixmap(drw->xc, drw_root_depth(drw->xc, drw->screen),
		    drw->drawable, drw->root, (uint16_t) w, (uint16_t) h);
		/* Recreate minimal GC bound to the new pixmap */
		xcb_free_gc(drw->xc, drw->gc);
		drw->gc = xcb_generate_id(drw->xc);
		xcb_create_gc(drw->xc, drw->gc, drw->drawable, 0, NULL);
	}

	/* Recreate Cairo surface for new pixmap */
	if (drw->cairo_surface)
		cairo_surface_destroy(drw->cairo_surface);
	drw->cairo_surface = NULL;
	if (drw->xcb_visual) {
		drw->cairo_surface = cairo_xcb_surface_create(drw->xc,
		    (xcb_drawable_t) drw->drawable, drw->xcb_visual, (int) w, (int) h);
		if (drw->cairo_surface &&
		    cairo_surface_status(drw->cairo_surface) != CAIRO_STATUS_SUCCESS) {
			cairo_surface_destroy(drw->cairo_surface);
			drw->cairo_surface = NULL;
		}
	}
}

void
drw_free(Drw *drw)
{
	if (drw->cairo_surface)
		cairo_surface_destroy(drw->cairo_surface);
	xcb_free_pixmap(drw->xc, drw->drawable);
	xcb_free_gc(drw->xc, drw->gc);
	drw_fontset_free(drw->fonts);
	free(drw);
}

/* ── fonts ──────────────────────────────────────────────────────────────── */

static Fnt *
xfont_create(Drw *drw, const char *fontname)
{
	Fnt                  *font;
	PangoFontDescription *desc;
	PangoContext         *ctx;
	PangoFontMetrics     *metrics;

	if (!fontname) {
		die("no font specified.");
	}

	desc = pango_font_description_from_string(fontname);
	if (!desc) {
		awm_error("cannot load font: '%s'", fontname);
		return NULL;
	}

	font       = ecalloc(1, sizeof(Fnt));
	font->desc = desc;

	if (!drw->cairo_surface) {
		awm_error(
		    "xfont_create: cairo surface not available for '%s'", fontname);
		pango_font_description_free(font->desc);
		free(font);
		return NULL;
	}

	{
		cairo_t *tmp_cr = cairo_create(drw->cairo_surface);
		ctx             = pango_cairo_create_context(tmp_cr);
		cairo_destroy(tmp_cr);
	}
	metrics = pango_context_get_metrics(ctx, desc, NULL);
	font->h = (unsigned int) ((pango_font_metrics_get_ascent(metrics) +
	                              pango_font_metrics_get_descent(metrics)) /
	    PANGO_SCALE);
	pango_font_metrics_unref(metrics);
	g_object_unref(ctx);

	return font;
}

static void
xfont_free(Fnt *font)
{
	if (!font)
		return;
	pango_font_description_free(font->desc);
	free(font);
}

Fnt *
drw_fontset_create(Drw *drw, const char *fonts[], size_t fontcount)
{
	Fnt   *cur, *ret = NULL;
	size_t i;

	if (!drw || !fonts)
		return NULL;

	for (i = 1; i <= fontcount; i++) {
		if ((cur = xfont_create(drw, fonts[fontcount - i]))) {
			cur->next = ret;
			ret       = cur;
		}
	}
	return (drw->fonts = ret);
}

void
drw_fontset_free(Fnt *font)
{
	if (font) {
		drw_fontset_free(font->next);
		xfont_free(font);
	}
}

unsigned int
drw_fontset_getwidth(Drw *drw, const char *text)
{
	if (!drw || !drw->fonts || !text)
		return 0;
	return (unsigned int) drw_text(drw, 0, 0, 0, 0, 0, text, 0);
}

unsigned int
drw_fontset_getwidth_clamp(Drw *drw, const char *text, unsigned int n)
{
	unsigned int tmp = 0;
	if (drw && drw->fonts && text && n)
		tmp = (unsigned int) drw_text(drw, 0, 0, 0, 0, 0, text, 0);
	return MIN(n, tmp);
}

/* ── colors ─────────────────────────────────────────────────────────────── */

void
drw_clr_create(Drw *drw, Clr *dest, const char *clrname)
{
	unsigned int r8 = 0, g8 = 0, b8 = 0;

	if (!drw || !dest || !clrname)
		return;

	/* Parse #rrggbb — the only format used in awm color configs. */
	if (clrname[0] != '#' ||
	    sscanf(clrname + 1, "%02x%02x%02x", &r8, &g8, &b8) != 3)
		die("error, cannot parse color '%s'", clrname);

	/* Expand 8-bit to 16-bit channels (0x101 * 0xff == 0xffff). */
	dest->r = (unsigned short) (r8 * 0x101);
	dest->g = (unsigned short) (g8 * 0x101);
	dest->b = (unsigned short) (b8 * 0x101);
	dest->a = 0xffff;

	/* pixel is unused in the Cairo backend — zero it so comparisons are safe
	 */
	dest->pixel = 0;
}

Clr *
drw_scm_create(Drw *drw, char *clrnames[], size_t clrcount)
{
	size_t i;
	Clr   *ret;

	if (!drw || !clrnames || clrcount < 2 ||
	    !(ret = ecalloc(clrcount, sizeof(Clr))))
		return NULL;

	for (i = 0; i < clrcount; i++)
		drw_clr_create(drw, &ret[i], clrnames[i]);
	return ret;
}

/* ── state setters ──────────────────────────────────────────────────────── */

void
drw_setfontset(Drw *drw, Fnt *set)
{
	if (drw)
		drw->fonts = set;
}

void
drw_setscheme(Drw *drw, Clr *scm)
{
	if (drw)
		drw->scheme = scm;
}

/* ── drawing ────────────────────────────────────────────────────────────── */

void
drw_rect(Drw *drw, int x, int y, unsigned int w, unsigned int h, int filled,
    int invert)
{
	Clr     *col;
	cairo_t *cr;

	if (!drw || !drw->scheme || !drw->cairo_surface)
		return;

	col = &drw->scheme[invert ? ColBg : ColFg];
	cr  = cairo_create(drw->cairo_surface);

	cairo_set_source_rgb(
	    cr, col->r / 65535.0, col->g / 65535.0, col->b / 65535.0);

	if (filled) {
		cairo_rectangle(cr, x, y, (double) w, (double) h);
		cairo_fill(cr);
	} else {
		/* 0.5px inset so the 1px stroke falls exactly on pixel boundaries,
		 * matching the visual result of xcb_poly_rectangle. */
		cairo_set_line_width(cr, 1.0);
		cairo_rectangle(
		    cr, x + 0.5, y + 0.5, (double) (w - 1), (double) (h - 1));
		cairo_stroke(cr);
	}

	cairo_destroy(cr);
}

int
drw_text(Drw *drw, int x, int y, unsigned int w, unsigned int h,
    unsigned int lpad, const char *text, int invert)
{
	int          render = x || y || w || h;
	PangoLayout *layout;
	cairo_t     *cr;
	int          tw, th;

	if (!drw || (render && (!drw->scheme || !w)) || !text || !drw->fonts)
		return 0;

	if (!drw->cairo_surface)
		return render ? x + (int) w : 0;

	cr     = cairo_create(drw->cairo_surface);
	layout = pango_cairo_create_layout(cr);
	pango_layout_set_font_description(layout, drw->fonts->desc);
	pango_layout_set_text(layout, text, -1);

	if (!render) {
		/* Measurement-only: return pixel width, nothing drawn */
		pango_layout_get_pixel_size(layout, &tw, NULL);
		g_object_unref(layout);
		cairo_destroy(cr);
		return tw;
	}

	/* Background fill — replaces xcb_poly_fill_rectangle + mark_dirty */
	{
		Clr *bg = &drw->scheme[invert ? ColFg : ColBg];
		cairo_set_source_rgb(
		    cr, bg->r / 65535.0, bg->g / 65535.0, bg->b / 65535.0);
		cairo_rectangle(cr, x, y, (double) w, (double) h);
		cairo_fill(cr);
	}

	/* Constrain width and ellipsize */
	{
		int avail = (lpad < w) ? (int) (w - lpad) : 0;
		pango_layout_set_width(layout, avail * PANGO_SCALE);
	}
	pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);

	pango_layout_get_pixel_size(layout, &tw, &th);

	/* Foreground colour */
	{
		Clr *fg = &drw->scheme[invert ? ColBg : ColFg];
		cairo_set_source_rgba(cr, fg->r / 65535.0, fg->g / 65535.0,
		    fg->b / 65535.0, fg->a / 65535.0);
	}

	/* Vertically centred */
	cairo_move_to(cr, x + (int) lpad, y + ((int) h - th) / 2);
	pango_cairo_show_layout(cr, layout);

	g_object_unref(layout);
	cairo_destroy(cr);

	return x + (int) w;
}

void
drw_pic(Drw *drw, int x, int y, unsigned int w, unsigned int h,
    cairo_surface_t *surface)
{
	int src_w, src_h;

	if (!drw || !surface || !w || !h)
		return;

	if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS)
		return;

	if (cairo_surface_get_type(surface) != CAIRO_SURFACE_TYPE_IMAGE) {
		awm_warn("drw_pic: non-image surface, icon skipped");
		return;
	}

	if (!drw->cairo_surface)
		return;

	src_w = cairo_image_surface_get_width(surface);
	src_h = cairo_image_surface_get_height(surface);
	if (src_w <= 0 || src_h <= 0)
		return;

	cairo_surface_flush(surface);

	cairo_t *cr = cairo_create(drw->cairo_surface);
	cairo_save(cr);
	cairo_translate(cr, x, y);
	if (src_w != (int) w || src_h != (int) h)
		cairo_scale(cr, (double) w / src_w, (double) h / src_h);
	cairo_set_source_surface(cr, surface, 0, 0);
	/* Default Cairo compositing operator is OVER — preserves alpha */
	cairo_paint(cr);
	cairo_restore(cr);
	cairo_destroy(cr);
}

/* ── map (blit to window) ───────────────────────────────────────────────── */

void
drw_map(
    Drw *drw, xcb_window_t win, int x, int y, unsigned int w, unsigned int h)
{
	if (!drw)
		return;

	if (drw->cairo_surface) {
		/* Flush all pending Cairo rendering to the XCB send buffer before
		 * xcb_copy_area.  Both share drw->xc so the server processes them
		 * in submission order — no race. */
		cairo_surface_flush(drw->cairo_surface);
	}

	xcb_copy_area(drw->xc, drw->drawable, (xcb_drawable_t) win, drw->gc,
	    (int16_t) x, (int16_t) y, (int16_t) x, (int16_t) y, (uint16_t) w,
	    (uint16_t) h);
	xcb_flush(drw->xc);
}

/* ── cursors ────────────────────────────────────────────────────────────── */

Cur *
drw_cur_create(Drw *drw, int shape)
{
	Cur *cur;

	if (!drw || !(cur = ecalloc(1, sizeof(Cur))))
		return NULL;

	{
		xcb_font_t font = xcb_generate_id(drw->xc);
		xcb_open_font(drw->xc, font, strlen("cursor"), "cursor");
		cur->cursor = xcb_generate_id(drw->xc);
		xcb_create_glyph_cursor(drw->xc, cur->cursor, font, font,
		    (uint16_t) shape, (uint16_t) (shape + 1), 0, 0, 0, 65535, 65535,
		    65535);
		xcb_close_font(drw->xc, font);
	}

	return cur;
}

void
drw_cur_free(Drw *drw, Cur *cursor)
{
	if (!cursor)
		return;

	xcb_free_cursor(drw->xc, cursor->cursor);
	free(cursor);
}
