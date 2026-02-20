/* See LICENSE file for copyright and license details. */
#include <X11/extensions/Xrender.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xcb/xcb.h>
#include <xcb/xproto.h>

#include <X11/Xlib-xcb.h>
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

Drw *
drw_create(
    Display *dpy, int screen, Window root, unsigned int w, unsigned int h)
{
	Drw *drw = ecalloc(1, sizeof(Drw));

	drw->dpy      = dpy;
	drw->screen   = screen;
	drw->root     = root;
	drw->w        = w;
	drw->h        = h;
	drw->drawable = XCreatePixmap(dpy, root, w, h, DefaultDepth(dpy, screen));
	drw->gc       = XCreateGC(dpy, root, 0, NULL);
	XSetLineAttributes(dpy, drw->gc, 1, LineSolid, CapButt, JoinMiter);

	/* Open a dedicated XCB connection for Cairo.  Cairo's xlib backend
	 * sends raw xcb_render_* requests on whatever xcb_connection_t it gets.
	 * Using a separate connection that is never read from via Xlib prevents
	 * the _XSetLastRequestRead "sequence lost" warning that fires whenever
	 * the wire sequence counter wraps past 0xffff on a shared Display*. */
	drw->cairo_xcb = xcb_connect(NULL, NULL);
	if (xcb_connection_has_error(drw->cairo_xcb)) {
		xcb_disconnect(drw->cairo_xcb);
		drw->cairo_xcb = NULL;
	}

	if (drw->cairo_xcb) {
		/* Walk XCB screen list to get root_visual for screen N,
		 * replacing XVisualIDFromVisual(DefaultVisual(dpy, screen)). */
		xcb_screen_iterator_t sit =
		    xcb_setup_roots_iterator(xcb_get_setup(drw->cairo_xcb));
		for (int i = 0; i < screen; i++)
			xcb_screen_next(&sit);
		drw->xcb_visual =
		    xcb_find_visualtype(drw->cairo_xcb, screen, sit.data->root_visual);
	}

	/* Create persistent Cairo surface for icon rendering */
	if (drw->cairo_xcb && drw->xcb_visual) {
		drw->cairo_surface = cairo_xcb_surface_create(drw->cairo_xcb,
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
	if (drw->drawable)
		XFreePixmap(drw->dpy, drw->drawable);
	drw->drawable = XCreatePixmap(
	    drw->dpy, drw->root, w, h, DefaultDepth(drw->dpy, drw->screen));

	/* Recreate Cairo surface for new drawable */
	if (drw->cairo_surface)
		cairo_surface_destroy(drw->cairo_surface);
	drw->cairo_surface = NULL;
	if (drw->cairo_xcb && drw->xcb_visual) {
		drw->cairo_surface = cairo_xcb_surface_create(drw->cairo_xcb,
		    (xcb_drawable_t) drw->drawable, drw->xcb_visual, (int) w, (int) h);
	}
}

void
drw_free(Drw *drw)
{
	if (drw->cairo_surface)
		cairo_surface_destroy(drw->cairo_surface);
	if (drw->cairo_xcb)
		xcb_disconnect(drw->cairo_xcb);
	XFreePixmap(drw->dpy, drw->drawable);
	XFreeGC(drw->dpy, drw->gc);
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
	xcb_connection_t        *xc;
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
	xc = XGetXCBConnection(drw->dpy);
	si = xcb_setup_roots_iterator(xcb_get_setup(xc));
	for (int i = 0; i < drw->screen; i++)
		xcb_screen_next(&si);
	xs  = si.data;
	ck  = xcb_alloc_color(xc, xs->default_colormap, dest->r, dest->g, dest->b);
	rep = xcb_alloc_color_reply(xc, ck, NULL);
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
	if (!drw || !drw->scheme)
		return;
	XSetForeground(drw->dpy, drw->gc,
	    invert ? drw->scheme[ColBg].pixel : drw->scheme[ColFg].pixel);
	if (filled)
		XFillRectangle(drw->dpy, drw->drawable, drw->gc, x, y, w, h);
	else
		XDrawRectangle(drw->dpy, drw->drawable, drw->gc, x, y, w - 1, h - 1);
	/* Notify Cairo that X11 has modified the drawable */
	if (drw->cairo_surface)
		cairo_surface_mark_dirty_rectangle(drw->cairo_surface, x, y, w, h);
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

	/* Fill background */
	XSetForeground(
	    drw->dpy, drw->gc, drw->scheme[invert ? ColFg : ColBg].pixel);
	XFillRectangle(drw->dpy, drw->drawable, drw->gc, x, y, w, h);
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
drw_map(Drw *drw, Window win, int x, int y, unsigned int w, unsigned int h)
{
	if (!drw)
		return;

	/* Flush any pending Cairo operations to the drawable before X copies it */
	if (drw->cairo_surface)
		cairo_surface_flush(drw->cairo_surface);

	XCopyArea(drw->dpy, drw->drawable, win, drw->gc, x, y, w, h, x, y);
	xcb_flush(XGetXCBConnection(drw->dpy));
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
		tmp = drw_text(drw, 0, 0, 0, 0, 0, text, n);
	return MIN(n, tmp);
}

Cur *
drw_cur_create(Drw *drw, int shape)
{
	Cur *cur;

	if (!drw || !(cur = ecalloc(1, sizeof(Cur))))
		return NULL;

	cur->cursor = XCreateFontCursor(drw->dpy, shape);

	return cur;
}

void
drw_cur_free(Drw *drw, Cur *cursor)
{
	if (!cursor)
		return;

	XFreeCursor(drw->dpy, cursor->cursor);
	free(cursor);
}

void
drw_pic(Drw *drw, int x, int y, unsigned int w, unsigned int h,
    cairo_surface_t *surface)
{
	int                src_w, src_h, stride;
	unsigned char     *data;
	Pixmap             tmp_pm;
	XImage            *img;
	Picture            src_pic, dst_pic;
	XRenderPictFormat *argb_fmt, *dst_fmt;
	Visual            *argb_vis = NULL;
	XVisualInfo        vi_tmpl;
	XVisualInfo       *vi_list;
	int                vi_count, i;

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

	/*
	 * Upload ARGB32 icon pixels into a temporary 32-bit depth pixmap and
	 * alpha-composite it into drw->drawable using XRenderComposite.
	 * Everything goes through the Xlib connection (dpy) — no separate XCB
	 * connection, no cross-connection ordering hazard.
	 */
	argb_fmt = XRenderFindStandardFormat(drw->dpy, PictStandardARGB32);
	if (!argb_fmt) {
		awm_warn("drw_pic: XRender ARGB32 format not available");
		return;
	}

	/* Find a 32-bit TrueColor visual for the temp pixmap */
	vi_tmpl.screen = drw->screen;
	vi_tmpl.depth  = 32;
	vi_tmpl.class  = TrueColor; /* c89-safe: 'class' is the field name */
	vi_list        = XGetVisualInfo(drw->dpy,
	           VisualScreenMask | VisualDepthMask | VisualClassMask, &vi_tmpl,
	           &vi_count);
	if (vi_list) {
		for (i = 0; i < vi_count; i++) {
			if (vi_list[i].depth == 32) {
				argb_vis = vi_list[i].visual;
				break;
			}
		}
		XFree(vi_list);
	}
	if (!argb_vis) {
		awm_warn("drw_pic: no 32-bit TrueColor visual, icon skipped");
		return;
	}

	tmp_pm = XCreatePixmap(
	    drw->dpy, drw->root, (unsigned) src_w, (unsigned) src_h, 32);

	img = XCreateImage(drw->dpy, argb_vis, 32, ZPixmap, 0, NULL,
	    (unsigned) src_w, (unsigned) src_h, 32, stride);
	if (!img) {
		XFreePixmap(drw->dpy, tmp_pm);
		return;
	}
	img->data = (char *) data; /* point at Cairo's buffer; we own it */

	/* Create a matching GC for the 32-bit pixmap */
	{
		GC gc32 = XCreateGC(drw->dpy, tmp_pm, 0, NULL);
		XPutImage(drw->dpy, tmp_pm, gc32, img, 0, 0, 0, 0, (unsigned) src_w,
		    (unsigned) src_h);
		XFreeGC(drw->dpy, gc32);
	}

	/* Detach data pointer so XDestroyImage won't free Cairo's buffer */
	img->data = NULL;
	XDestroyImage(img);

	src_pic = XRenderCreatePicture(drw->dpy, tmp_pm, argb_fmt, 0, NULL);

	dst_fmt = XRenderFindVisualFormat(
	    drw->dpy, DefaultVisual(drw->dpy, drw->screen));
	dst_pic = XRenderCreatePicture(drw->dpy, drw->drawable, dst_fmt, 0, NULL);

	if ((unsigned) src_w != w || (unsigned) src_h != h) {
		XTransform xform = { { { XDoubleToFixed((double) src_w / w), 0, 0 },
			{ 0, XDoubleToFixed((double) src_h / h), 0 },
			{ 0, 0, XDoubleToFixed(1.0) } } };
		XRenderSetPictureTransform(drw->dpy, src_pic, &xform);
		XRenderSetPictureFilter(drw->dpy, src_pic, FilterBilinear, NULL, 0);
	}

	XRenderComposite(
	    drw->dpy, PictOpOver, src_pic, None, dst_pic, 0, 0, 0, 0, x, y, w, h);

	XRenderFreePicture(drw->dpy, src_pic);
	XRenderFreePicture(drw->dpy, dst_pic);
	XFreePixmap(drw->dpy, tmp_pm);
}
