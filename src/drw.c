/* See LICENSE file for copyright and license details. */
#include <stdlib.h>
#include <string.h>
#include <xcb/xcb.h>
#include <xcb/xproto.h>
#include <xcb/render.h>
#include <xcb/xcb_renderutil.h>
#include <xcb/xcb_cursor.h>
#include <pango/pangocairo.h>

#include "drw.h"
#include "log.h"
#include "util.h"

/* Find the xcb_visualtype_t matching a given visual ID on the given screen.
 * Returns NULL if not found (should never happen for DefaultVisual). */
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

/* Return the root depth for screen number scr_num. */
static uint8_t
drw_root_depth(xcb_connection_t *conn, int scr_num)
{
	xcb_screen_iterator_t it = xcb_setup_roots_iterator(xcb_get_setup(conn));
	for (int i = 0; i < scr_num; i++)
		xcb_screen_next(&it);
	return it.data->root_depth;
}

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

	/* Create the backing pixmap */
	drw->drawable = xcb_generate_id(xc);
	xcb_create_pixmap(xc, drw_root_depth(xc, screen), drw->drawable, root,
	    (uint16_t) w, (uint16_t) h);

	/* Create a GC on the pixmap */
	drw->gc = xcb_generate_id(xc);
	{
		uint32_t mask = XCB_GC_LINE_WIDTH | XCB_GC_LINE_STYLE |
		    XCB_GC_CAP_STYLE | XCB_GC_JOIN_STYLE;
		uint32_t vals[4] = { 1, XCB_LINE_STYLE_SOLID, XCB_CAP_STYLE_BUTT,
			XCB_JOIN_STYLE_MITER };
		xcb_create_gc(xc, drw->gc, drw->drawable, mask, vals);
	}

	/* Use the main XCB connection for Cairo.  A single connection removes the
	 * two-connection race where xcb_copy_area could run before Cairo finishes
	 * rendering into the pixmap.  cairo_surface_flush() in drw_map() ensures
	 * all pending Cairo requests have been sent before xcb_copy_area. */
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
		/* Reattach GC to new drawable */
		xcb_free_gc(drw->xc, drw->gc);
		drw->gc = xcb_generate_id(drw->xc);
		{
			uint32_t mask = XCB_GC_LINE_WIDTH | XCB_GC_LINE_STYLE |
			    XCB_GC_CAP_STYLE | XCB_GC_JOIN_STYLE;
			uint32_t vals[4] = { 1, XCB_LINE_STYLE_SOLID, XCB_CAP_STYLE_BUTT,
				XCB_JOIN_STYLE_MITER };
			xcb_create_gc(drw->xc, drw->gc, drw->drawable, mask, vals);
		}
	}

	/* Recreate Cairo surface for new drawable */
	if (drw->cairo_surface)
		cairo_surface_destroy(drw->cairo_surface);
	drw->cairo_surface = NULL;
	if (drw->xcb_visual) {
		drw->cairo_surface = cairo_xcb_surface_create(drw->xc,
		    (xcb_drawable_t) drw->drawable, drw->xcb_visual, (int) w, (int) h);
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

/* This function is an implementation detail. Library users should use
 * drw_fontset_create instead.
 */
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

	/* Measure line height using a temporary PangoContext on the Cairo surface
	 */
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

void
drw_clr_create(Drw *drw, Clr *dest, const char *clrname)
{
	xcb_screen_iterator_t    si;
	xcb_screen_t            *xs;
	xcb_alloc_color_cookie_t ck;
	xcb_alloc_color_reply_t *rep;
	unsigned int             r8 = 0, g8 = 0, b8 = 0;

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

	/* Allocate pixel value in the server's colormap via XCB. */
	si = xcb_setup_roots_iterator(xcb_get_setup(drw->xc));
	for (int i = 0; i < drw->screen; i++)
		xcb_screen_next(&si);
	xs = si.data;
	ck = xcb_alloc_color(
	    drw->xc, xs->default_colormap, dest->r, dest->g, dest->b);
	rep = xcb_alloc_color_reply(drw->xc, ck, NULL);
	if (!rep)
		die("error, cannot allocate color '%s'", clrname);
	dest->pixel = rep->pixel;
	free(rep);
}

/* Wrapper to create color schemes. The caller has to call free(3) on the
 * returned color scheme when done using it. */
Clr *
drw_scm_create(Drw *drw, char *clrnames[], size_t clrcount)
{
	size_t i;
	Clr   *ret;

	/* need at least two colors for a scheme */
	if (!drw || !clrnames || clrcount < 2 ||
	    !(ret = ecalloc(clrcount, sizeof(Clr))))
		return NULL;

	for (i = 0; i < clrcount; i++)
		drw_clr_create(drw, &ret[i], clrnames[i]);
	return ret;
}

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

void
drw_rect(Drw *drw, int x, int y, unsigned int w, unsigned int h, int filled,
    int invert)
{
	uint32_t col;

	if (!drw || !drw->scheme)
		return;

	col = (uint32_t) (invert ? drw->scheme[ColBg].pixel
	                         : drw->scheme[ColFg].pixel);
	xcb_change_gc(drw->xc, drw->gc, XCB_GC_FOREGROUND, &col);
	if (filled) {
		xcb_rectangle_t r = { (int16_t) x, (int16_t) y, (uint16_t) w,
			(uint16_t) h };
		xcb_poly_fill_rectangle(drw->xc, drw->drawable, drw->gc, 1, &r);
	} else {
		xcb_rectangle_t r = { (int16_t) x, (int16_t) y, (uint16_t) (w - 1),
			(uint16_t) (h - 1) };
		xcb_poly_rectangle(drw->xc, drw->drawable, drw->gc, 1, &r);
	}
	/* Notify Cairo that X11 has modified the drawable */
	if (drw->cairo_surface)
		cairo_surface_mark_dirty_rectangle(
		    drw->cairo_surface, x, y, (int) w, (int) h);
}

int
drw_text(Drw *drw, int x, int y, unsigned int w, unsigned int h,
    unsigned int lpad, const char *text, int invert)
{
	int          render = x || y || w || h;
	PangoLayout *layout;
	cairo_t     *cr;
	int          tw, th;
	uint32_t     col;

	if (!drw || (render && (!drw->scheme || !w)) || !text || !drw->fonts)
		return 0;

	if (!render) {
		/* measurement-only mode: return pixel width of text */
		if (!drw->cairo_surface)
			return 0;
		cr     = cairo_create(drw->cairo_surface);
		layout = pango_cairo_create_layout(cr);
		pango_layout_set_font_description(layout, drw->fonts->desc);
		pango_layout_set_text(layout, text, -1);
		pango_layout_get_pixel_size(layout, &tw, NULL);
		g_object_unref(layout);
		cairo_destroy(cr);
		return tw;
	}

	/* Fill background via XCB */
	col = (uint32_t) drw->scheme[invert ? ColFg : ColBg].pixel;
	xcb_change_gc(drw->xc, drw->gc, XCB_GC_FOREGROUND, &col);
	{
		xcb_rectangle_t r = { (int16_t) x, (int16_t) y, (uint16_t) w,
			(uint16_t) h };
		xcb_poly_fill_rectangle(drw->xc, drw->drawable, drw->gc, 1, &r);
	}
	if (drw->cairo_surface)
		cairo_surface_mark_dirty_rectangle(
		    drw->cairo_surface, x, y, (int) w, (int) h);

	if (!drw->cairo_surface)
		return x + (int) w;

	/* Render text via PangoCairo */
	cr     = cairo_create(drw->cairo_surface);
	layout = pango_cairo_create_layout(cr);
	pango_layout_set_font_description(layout, drw->fonts->desc);
	pango_layout_set_text(layout, text, -1);

	/* Ellipsize if text exceeds available width */
	pango_layout_set_width(layout, (int) (w - lpad) * PANGO_SCALE);
	pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);

	pango_layout_get_pixel_size(layout, &tw, &th);

	/* Set foreground colour */
	{
		Clr *fg = &drw->scheme[invert ? ColBg : ColFg];
		cairo_set_source_rgba(cr, fg->r / 65535.0, fg->g / 65535.0,
		    fg->b / 65535.0, fg->a / 65535.0);
	}

	/* Vertically centre */
	cairo_move_to(cr, x + (int) lpad, y + ((int) h - th) / 2);
	pango_cairo_show_layout(cr, layout);

	g_object_unref(layout);
	cairo_destroy(cr);

	return x + (int) w;
}

void
drw_map(
    Drw *drw, xcb_window_t win, int x, int y, unsigned int w, unsigned int h)
{
	if (!drw)
		return;

	if (drw->cairo_surface) {
		/* Flush all pending Cairo/Pango rendering to the XCB send buffer
		 * before issuing xcb_copy_area.  Both Cairo and the XCB background
		 * fills share the same connection (drw->xc), so the X server will
		 * process them in submission order — no cross-connection race. */
		cairo_surface_flush(drw->cairo_surface);
	}

	xcb_copy_area(drw->xc, drw->drawable, (xcb_drawable_t) win, drw->gc,
	    (int16_t) x, (int16_t) y, (int16_t) x, (int16_t) y, (uint16_t) w,
	    (uint16_t) h);
	xcb_flush(drw->xc);
}

unsigned int
drw_fontset_getwidth(Drw *drw, const char *text)
{
	if (!drw || !drw->fonts || !text)
		return 0;
	return drw_text(drw, 0, 0, 0, 0, 0, text, 0);
}

unsigned int
drw_fontset_getwidth_clamp(Drw *drw, const char *text, unsigned int n)
{
	unsigned int tmp = 0;
	if (drw && drw->fonts && text && n)
		tmp = drw_text(drw, 0, 0, 0, 0, 0, text, 0);
	return MIN(n, tmp);
}

Cur *
drw_cur_create(Drw *drw, int shape)
{
	Cur                  *cur;
	xcb_cursor_context_t *ctx;

	if (!drw || !(cur = ecalloc(1, sizeof(Cur))))
		return NULL;

	/* Use xcb-cursor to load a cursor from the cursor font by glyph index
	 */
	if (xcb_cursor_context_new(drw->xc,
	        xcb_setup_roots_iterator(xcb_get_setup(drw->xc)).data, &ctx) < 0) {
		/* Fallback: create glyph cursor directly from cursor font */
		xcb_font_t font = xcb_generate_id(drw->xc);
		xcb_open_font(drw->xc, font, strlen("cursor"), "cursor");
		cur->cursor = xcb_generate_id(drw->xc);
		xcb_create_glyph_cursor(drw->xc, cur->cursor, font, font,
		    (uint16_t) shape, (uint16_t) (shape + 1), 0, 0, 0, 65535, 65535,
		    65535);
		xcb_close_font(drw->xc, font);
		return cur;
	}

	xcb_cursor_context_free(ctx);

	/* Create glyph cursor directly — reliable and matches Xlib behaviour
	 */
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

void
drw_pic(Drw *drw, int x, int y, unsigned int w, unsigned int h,
    cairo_surface_t *surface)
{
	int                     src_w, src_h, stride;
	unsigned char          *data;
	xcb_pixmap_t            tmp_pm;
	xcb_render_picture_t    src_pic, dst_pic;
	xcb_render_pictformat_t argb_fmt, dst_fmt;
	xcb_visualid_t          argb_vis = XCB_NONE;

	if (!drw || !surface)
		return;

	if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS)
		return;

	if (cairo_surface_get_type(surface) != CAIRO_SURFACE_TYPE_IMAGE) {
		awm_warn("drw_pic: non-image surface, icon skipped");
		return;
	}

	cairo_surface_flush(surface);
	data   = cairo_image_surface_get_data(surface);
	src_w  = cairo_image_surface_get_width(surface);
	src_h  = cairo_image_surface_get_height(surface);
	stride = cairo_image_surface_get_stride(surface);

	if (!data || src_w <= 0 || src_h <= 0)
		return;

	/* Look up the ARGB32 picture format */
	{
		xcb_render_query_pict_formats_cookie_t ck =
		    xcb_render_query_pict_formats(drw->xc);
		xcb_render_query_pict_formats_reply_t *reply =
		    xcb_render_query_pict_formats_reply(drw->xc, ck, NULL);
		if (!reply) {
			awm_warn("drw_pic: xcb_render_query_pict_formats failed");
			return;
		}
		xcb_render_pictforminfo_t argb_want = { 0 };
		argb_want.type                      = XCB_RENDER_PICT_TYPE_DIRECT;
		argb_want.depth                     = 32;
		argb_want.direct.red_shift          = 16;
		argb_want.direct.red_mask           = 0xff;
		argb_want.direct.green_shift        = 8;
		argb_want.direct.green_mask         = 0xff;
		argb_want.direct.blue_shift         = 0;
		argb_want.direct.blue_mask          = 0xff;
		argb_want.direct.alpha_shift        = 24;
		argb_want.direct.alpha_mask         = 0xff;
		xcb_render_pictforminfo_t *argb_fi = xcb_render_util_find_format(reply,
		    XCB_PICT_FORMAT_TYPE | XCB_PICT_FORMAT_DEPTH |
		        XCB_PICT_FORMAT_RED | XCB_PICT_FORMAT_RED_MASK |
		        XCB_PICT_FORMAT_GREEN | XCB_PICT_FORMAT_GREEN_MASK |
		        XCB_PICT_FORMAT_BLUE | XCB_PICT_FORMAT_BLUE_MASK |
		        XCB_PICT_FORMAT_ALPHA | XCB_PICT_FORMAT_ALPHA_MASK,
		    &argb_want, 0);
		if (!argb_fi) {
			awm_warn("drw_pic: ARGB32 picture format not found");
			free(reply);
			return;
		}
		argb_fmt = argb_fi->id;

		/* Find a 32-bit TrueColor visual for the temp pixmap */
		xcb_depth_iterator_t  di;
		xcb_screen_iterator_t si =
		    xcb_setup_roots_iterator(xcb_get_setup(drw->xc));
		for (int i = 0; i < drw->screen; i++)
			xcb_screen_next(&si);
		di = xcb_screen_allowed_depths_iterator(si.data);
		for (; di.rem && argb_vis == XCB_NONE; xcb_depth_next(&di)) {
			if (di.data->depth != 32)
				continue;
			xcb_visualtype_iterator_t vi = xcb_depth_visuals_iterator(di.data);
			if (vi.rem)
				argb_vis = vi.data->visual_id;
		}

		/* Find the dst format matching the screen root visual */
		xcb_render_pictvisual_t *dst_pv =
		    xcb_render_util_find_visual_format(reply, si.data->root_visual);
		dst_fmt = dst_pv ? dst_pv->format : argb_fmt;

		free(reply);
	}

	if (argb_vis == XCB_NONE) {
		awm_warn("drw_pic: no 32-bit TrueColor visual, icon skipped");
		return;
	}

	/* Create a temporary 32-bit pixmap and upload the ARGB pixel data */
	tmp_pm = xcb_generate_id(drw->xc);
	xcb_create_pixmap(
	    drw->xc, 32, tmp_pm, drw->root, (uint16_t) src_w, (uint16_t) src_h);

	/* Need a GC matched to tmp_pm's depth (32-bit) — drw->gc is root-depth
	 */
	{
		xcb_gcontext_t gc32 = xcb_generate_id(drw->xc);
		xcb_create_gc(drw->xc, gc32, tmp_pm, 0, NULL);
		xcb_put_image(drw->xc, XCB_IMAGE_FORMAT_Z_PIXMAP, tmp_pm, gc32,
		    (uint16_t) src_w, (uint16_t) src_h, 0, 0, 0, 32,
		    (uint32_t) (stride * src_h), data);
		xcb_free_gc(drw->xc, gc32);
	}

	/* Create XRender pictures */
	src_pic = xcb_generate_id(drw->xc);
	xcb_render_create_picture(drw->xc, src_pic, tmp_pm, argb_fmt, 0, NULL);

	dst_pic = xcb_generate_id(drw->xc);
	xcb_render_create_picture(
	    drw->xc, dst_pic, drw->drawable, dst_fmt, 0, NULL);

	/* Scale if needed */
	if ((unsigned) src_w != w || (unsigned) src_h != h) {
		xcb_render_fixed_t sx =
		    (xcb_render_fixed_t) ((double) src_w / w * 65536.0 + 0.5);
		xcb_render_fixed_t sy =
		    (xcb_render_fixed_t) ((double) src_h / h * 65536.0 + 0.5);
		xcb_render_fixed_t     one   = (xcb_render_fixed_t) 65536;
		xcb_render_transform_t xform = { sx, 0, 0, 0, sy, 0, 0, 0, one };
		xcb_render_set_picture_transform(drw->xc, src_pic, xform);
		xcb_render_set_picture_filter(
		    drw->xc, src_pic, strlen("bilinear"), "bilinear", 0, NULL);
	}

	xcb_render_composite(drw->xc, XCB_RENDER_PICT_OP_OVER, src_pic, XCB_NONE,
	    dst_pic, 0, 0, 0, 0, (int16_t) x, (int16_t) y, (uint16_t) w,
	    (uint16_t) h);

	xcb_render_free_picture(drw->xc, src_pic);
	xcb_render_free_picture(drw->xc, dst_pic);
	xcb_free_pixmap(drw->xc, tmp_pm);
}
