/* See LICENSE file for copyright and license details.
 *
 * render_cairo_xcb.c — Cairo/XCB render backend; implements RenderBackend.
 *
 * AwmSurface is Drw — defined here as the opaque concrete type.
 * The vtable render_backend_cairo_xcb is the sole exported singleton.
 */

#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <xcb/xcb.h>
#include <xcb/xproto.h>
#include <xcb/xcb_cursor.h>
#include <pango/pangocairo.h>

#include "awm.h"
#include "render.h"
#include "log.h"
#include "util.h"

/* ── Drw (concrete AwmSurface) ──────────────────────────────────────────── */

struct Drw {
	unsigned int      w, h;
	xcb_connection_t *xc; /* main XCB connection (shared, not owned) */
	int               screen;
	xcb_window_t      root;
	xcb_pixmap_t      drawable;
	xcb_gcontext_t    gc;
	Clr              *scheme;
	Fnt              *fonts;
	xcb_visualtype_t *xcb_visual;
	cairo_surface_t  *cairo_surface;
};

/* Global vtable pointer — set in setup() */
RenderBackend *g_render_backend = NULL;

/* ── internal helpers ───────────────────────────────────────────────────── */

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

static AwmSurface *
render_create(xcb_connection_t *xc, int screen, xcb_window_t root,
    unsigned int w, unsigned int h)
{
	AwmSurface *s = ecalloc(1, sizeof(AwmSurface));

	s->xc     = xc;
	s->screen = screen;
	s->root   = root;
	s->w      = w;
	s->h      = h;

	s->drawable = xcb_generate_id(xc);
	xcb_create_pixmap(xc, drw_root_depth(xc, screen), s->drawable, root,
	    (uint16_t) w, (uint16_t) h);

	s->gc = xcb_generate_id(xc);
	xcb_create_gc(xc, s->gc, s->drawable, 0, NULL);

	{
		xcb_screen_iterator_t sit =
		    xcb_setup_roots_iterator(xcb_get_setup(xc));
		for (int i = 0; i < screen; i++)
			xcb_screen_next(&sit);
		s->xcb_visual = xcb_find_visualtype(xc, screen, sit.data->root_visual);
	}

	if (s->xcb_visual) {
		s->cairo_surface = cairo_xcb_surface_create(
		    xc, (xcb_drawable_t) s->drawable, s->xcb_visual, (int) w, (int) h);
		if (s->cairo_surface &&
		    cairo_surface_status(s->cairo_surface) != CAIRO_STATUS_SUCCESS) {
			cairo_surface_destroy(s->cairo_surface);
			s->cairo_surface = NULL;
		}
	}

	return s;
}

static void render_fontset_free(Fnt *font); /* forward */

static void
render_resize(AwmSurface *s, unsigned int w, unsigned int h)
{
	if (!s)
		return;

	s->w = w;
	s->h = h;

	if (s->drawable) {
		xcb_free_pixmap(s->xc, s->drawable);
		s->drawable = xcb_generate_id(s->xc);
		xcb_create_pixmap(s->xc, drw_root_depth(s->xc, s->screen), s->drawable,
		    s->root, (uint16_t) w, (uint16_t) h);
		xcb_free_gc(s->xc, s->gc);
		s->gc = xcb_generate_id(s->xc);
		xcb_create_gc(s->xc, s->gc, s->drawable, 0, NULL);
	}

	if (s->cairo_surface)
		cairo_surface_destroy(s->cairo_surface);
	s->cairo_surface = NULL;
	if (s->xcb_visual) {
		s->cairo_surface = cairo_xcb_surface_create(s->xc,
		    (xcb_drawable_t) s->drawable, s->xcb_visual, (int) w, (int) h);
		if (s->cairo_surface &&
		    cairo_surface_status(s->cairo_surface) != CAIRO_STATUS_SUCCESS) {
			cairo_surface_destroy(s->cairo_surface);
			s->cairo_surface = NULL;
		}
	}
}

static void
render_free(AwmSurface *s)
{
	assert(s != NULL);
	if (s->cairo_surface)
		cairo_surface_destroy(s->cairo_surface);
	xcb_free_pixmap(s->xc, s->drawable);
	xcb_free_gc(s->xc, s->gc);
	render_fontset_free(s->fonts);
	free(s);
}

/* ── fonts ──────────────────────────────────────────────────────────────── */

static Fnt *
xfont_create(AwmSurface *s, const char *fontname)
{
	Fnt                  *font;
	PangoFontDescription *desc;
	PangoContext         *ctx;
	PangoFontMetrics     *metrics;

	if (!fontname)
		die("no font specified.");

	desc = pango_font_description_from_string(fontname);
	if (!desc) {
		awm_error("cannot load font: '%s'", fontname);
		return NULL;
	}

	font       = ecalloc(1, sizeof(Fnt));
	font->desc = desc;

	if (!s->cairo_surface) {
		awm_error(
		    "xfont_create: cairo surface not available for '%s'", fontname);
		pango_font_description_free(font->desc);
		free(font);
		return NULL;
	}

	{
		cairo_t *tmp_cr = cairo_create(s->cairo_surface);
		ctx             = pango_cairo_create_context(tmp_cr);
		cairo_destroy(tmp_cr);
	}
	if (g_plat.ui_dpi > 0.0)
		pango_cairo_context_set_resolution(ctx, g_plat.ui_dpi);
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

static Fnt *
render_fontset_create(AwmSurface *s, const char **fonts, size_t fontcount)
{
	Fnt   *cur, *ret = NULL;
	size_t i;

	if (!s || !fonts)
		return NULL;

	for (i = 1; i <= fontcount; i++) {
		if ((cur = xfont_create(s, fonts[fontcount - i]))) {
			cur->next = ret;
			ret       = cur;
		}
	}
	return (s->fonts = ret);
}

static void
render_fontset_free(Fnt *font)
{
	if (font) {
		render_fontset_free(font->next);
		xfont_free(font);
	}
}

/* forward declaration for getwidth */
static int render_text(AwmSurface *s, int x, int y, unsigned int w,
    unsigned int h, unsigned int lpad, const char *text, int invert);

static unsigned int
render_fontset_getwidth(AwmSurface *s, const char *text)
{
	if (!s || !s->fonts || !text)
		return 0;
	return (unsigned int) render_text(s, 0, 0, 0, 0, 0, text, 0);
}

static unsigned int
render_fontset_getwidth_clamp(AwmSurface *s, const char *text, unsigned int n)
{
	unsigned int tmp = 0;

	if (s && s->fonts && text && n)
		tmp = (unsigned int) render_text(s, 0, 0, 0, 0, 0, text, 0);
	return MIN(n, tmp);
}

/* ── colors ─────────────────────────────────────────────────────────────── */

static void
render_clr_create(AwmSurface *s, Clr *dest, const char *clrname)
{
	unsigned int r8 = 0, g8 = 0, b8 = 0;

	if (!s || !dest || !clrname)
		return;

	if (clrname[0] != '#' ||
	    sscanf(clrname + 1, "%02x%02x%02x", &r8, &g8, &b8) != 3)
		die("error, cannot parse color '%s'", clrname);

	dest->r     = (unsigned short) (r8 * 0x101);
	dest->g     = (unsigned short) (g8 * 0x101);
	dest->b     = (unsigned short) (b8 * 0x101);
	dest->a     = 0xffff;
	dest->pixel = 0;
}

static Clr *
render_scm_create(AwmSurface *s, char **clrnames, size_t clrcount)
{
	size_t i;
	Clr   *ret;

	if (!s || !clrnames || clrcount < 2 ||
	    !(ret = ecalloc(clrcount, sizeof(Clr))))
		return NULL;

	for (i = 0; i < clrcount; i++)
		render_clr_create(s, &ret[i], clrnames[i]);
	return ret;
}

/* ── state setters ──────────────────────────────────────────────────────── */

static void
render_setfontset(AwmSurface *s, Fnt *set)
{
	if (s)
		s->fonts = set;
}

static void
render_setscheme(AwmSurface *s, Clr *scm)
{
	if (s)
		s->scheme = scm;
}

/* ── drawing ────────────────────────────────────────────────────────────── */

static void
render_rect(AwmSurface *s, int x, int y, unsigned int w, unsigned int h,
    int filled, int invert)
{
	Clr     *col;
	cairo_t *cr;

	if (!s || !s->scheme || !s->cairo_surface)
		return;

	col = &s->scheme[invert ? ColBg : ColFg];
	cr  = cairo_create(s->cairo_surface);

	cairo_set_source_rgb(
	    cr, col->r / 65535.0, col->g / 65535.0, col->b / 65535.0);

	if (filled) {
		cairo_rectangle(cr, x, y, (double) w, (double) h);
		cairo_fill(cr);
	} else {
		cairo_set_line_width(cr, 1.0);
		cairo_rectangle(
		    cr, x + 0.5, y + 0.5, (double) (w - 1), (double) (h - 1));
		cairo_stroke(cr);
	}

	cairo_destroy(cr);
}

static int
render_text(AwmSurface *s, int x, int y, unsigned int w, unsigned int h,
    unsigned int lpad, const char *text, int invert)
{
	int          render = x || y || w || h;
	PangoLayout *layout;
	cairo_t     *cr;
	int          tw, th;

	if (!s || (render && (!s->scheme || !w)) || !text || !s->fonts)
		return 0;

	if (!s->cairo_surface)
		return render ? x + (int) w : 0;

	cr     = cairo_create(s->cairo_surface);
	layout = pango_cairo_create_layout(cr);
	if (g_plat.ui_dpi > 0.0)
		pango_cairo_context_set_resolution(
		    pango_layout_get_context(layout), g_plat.ui_dpi);
	pango_layout_set_font_description(layout, s->fonts->desc);
	pango_layout_set_text(layout, text, -1);

	if (!render) {
		pango_layout_get_pixel_size(layout, &tw, NULL);
		g_object_unref(layout);
		cairo_destroy(cr);
		return tw;
	}

	{
		Clr *bg = &s->scheme[invert ? ColFg : ColBg];
		cairo_set_source_rgb(
		    cr, bg->r / 65535.0, bg->g / 65535.0, bg->b / 65535.0);
		cairo_rectangle(cr, x, y, (double) w, (double) h);
		cairo_fill(cr);
	}

	{
		int avail = (lpad < w) ? (int) (w - lpad) : 0;
		pango_layout_set_width(layout, avail * PANGO_SCALE);
	}
	pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);

	pango_layout_get_pixel_size(layout, &tw, &th);

	{
		Clr *fg = &s->scheme[invert ? ColBg : ColFg];
		cairo_set_source_rgba(cr, fg->r / 65535.0, fg->g / 65535.0,
		    fg->b / 65535.0, fg->a / 65535.0);
	}

	cairo_move_to(cr, x + (int) lpad, y + ((int) h - th) / 2);
	pango_cairo_show_layout(cr, layout);

	g_object_unref(layout);
	cairo_destroy(cr);

	return x + (int) w;
}

static void
render_pic(AwmSurface *s, int x, int y, unsigned int w, unsigned int h,
    cairo_surface_t *surface)
{
	int src_w, src_h;

	if (!s || !surface || !w || !h)
		return;

	if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS)
		return;

	if (cairo_surface_get_type(surface) != CAIRO_SURFACE_TYPE_IMAGE) {
		awm_warn("render_pic: non-image surface, icon skipped");
		return;
	}

	if (!s->cairo_surface)
		return;

	src_w = cairo_image_surface_get_width(surface);
	src_h = cairo_image_surface_get_height(surface);
	if (src_w <= 0 || src_h <= 0)
		return;

	cairo_surface_flush(surface);

	{
		cairo_t *cr = cairo_create(s->cairo_surface);
		cairo_save(cr);
		cairo_translate(cr, x, y);
		if (src_w != (int) w || src_h != (int) h)
			cairo_scale(cr, (double) w / src_w, (double) h / src_h);
		cairo_set_source_surface(cr, surface, 0, 0);
		cairo_paint(cr);
		cairo_restore(cr);
		cairo_destroy(cr);
	}
}

/* ── status2d escape-code renderer ─────────────────────────────────────── */

static int
render_draw_statusd(AwmSurface *s, int x, int y, unsigned int w,
    unsigned int h, const char *text)
{
	int         render = x || y || w || h;
	double      cfg_r, cfg_g, cfg_b;
	double      cbg_r, cbg_g, cbg_b;
	int         cx;
	const char *p, *seg;
	char        buf[256];
	int         seg_len;
	cairo_t    *cr       = NULL;
	int         consumed = 0;

	if (!s || !s->scheme || !text)
		return 0;
	if (render && !s->cairo_surface)
		return 0;

	cfg_r = s->scheme[ColFg].r / 65535.0;
	cfg_g = s->scheme[ColFg].g / 65535.0;
	cfg_b = s->scheme[ColFg].b / 65535.0;
	cbg_r = s->scheme[ColBg].r / 65535.0;
	cbg_g = s->scheme[ColBg].g / 65535.0;
	cbg_b = s->scheme[ColBg].b / 65535.0;

	cx  = x;
	p   = text;
	seg = text;

	if (render)
		cr = cairo_create(s->cairo_surface);

	while (*p) {
		if (*p != '^') {
			p++;
			continue;
		}

		seg_len = (int) (p - seg);
		if (seg_len > 0 && (!render || (unsigned int) (cx - x) < w)) {
			if (seg_len >= (int) sizeof(buf))
				seg_len = (int) sizeof(buf) - 1;
			memcpy(buf, seg, (size_t) seg_len);
			buf[seg_len] = '\0';
			{
				PangoLayout          *layout;
				PangoFontDescription *desc;
				int                   tw, th;
				cairo_t              *tmp_cr;

				if (render) {
					tmp_cr = cr;
				} else {
					tmp_cr = cairo_create(s->cairo_surface);
				}

				layout = pango_cairo_create_layout(tmp_cr);
				if (g_plat.ui_dpi > 0.0)
					pango_cairo_context_set_resolution(
					    pango_layout_get_context(layout), g_plat.ui_dpi);
				desc = s->fonts ? s->fonts->desc : NULL;
				if (desc)
					pango_layout_set_font_description(layout, desc);
				pango_layout_set_text(layout, buf, -1);
				pango_layout_get_pixel_size(layout, &tw, &th);

				if (render) {
					cairo_set_source_rgb(cr, cbg_r, cbg_g, cbg_b);
					cairo_rectangle(cr, cx, y,
					    (double) (tw + g_plat.lrpad / 2), (double) h);
					cairo_fill(cr);
					cairo_set_source_rgb(cr, cfg_r, cfg_g, cfg_b);
					cairo_move_to(
					    cr, cx + g_plat.lrpad / 2, y + ((int) h - th) / 2);
					pango_cairo_show_layout(cr, layout);
				} else {
					cairo_destroy(tmp_cr);
				}
				g_object_unref(layout);

				cx += tw + g_plat.lrpad / 2;
				consumed += tw + g_plat.lrpad / 2;
			}
		}

		p++;

		if (*p == '^') {
			seg = p;
			p++;
			continue;
		}

		if ((*p == 'c' || *p == 'b') && p[1] != '^') {
			char         cmd   = *p++;
			int          ishex = (*p == '#');
			unsigned int rv = 0, gv = 0, bv = 0;
			const char  *q = p + ishex;

			if (sscanf(q, "%2x%2x%2x", &rv, &gv, &bv) == 3) {
				double dr = rv / 255.0;
				double dg = gv / 255.0;
				double db = bv / 255.0;
				if (cmd == 'c') {
					cfg_r = dr;
					cfg_g = dg;
					cfg_b = db;
				} else {
					cbg_r = dr;
					cbg_g = dg;
					cbg_b = db;
				}
			}
			while (*p && *p != '^')
				p++;
			if (*p == '^')
				p++;
		} else if (*p == 'r') {
			int rx = 0, ry = 0, rw = 0, rh = 0;
			p++;
			while (*p == ' ')
				p++;
			sscanf(p, "%d,%d,%d,%d", &rx, &ry, &rw, &rh);
			while (*p && *p != '^')
				p++;
			if (*p == '^')
				p++;
			if (render && rw > 0 && rh > 0) {
				cairo_set_source_rgb(cr, cfg_r, cfg_g, cfg_b);
				cairo_rectangle(cr, cx + rx, y + ry, (double) rw, (double) rh);
				cairo_fill(cr);
			}
		} else if (*p == 'f') {
			int fwd = 0;
			p++;
			while (*p == ' ')
				p++;
			if (*p >= '0' && *p <= '9') {
				fwd = atoi(p);
				while (*p >= '0' && *p <= '9')
					p++;
			}
			while (*p && *p != '^')
				p++;
			if (*p == '^')
				p++;
			cx += fwd;
			consumed += fwd;
		} else if (*p == 'd') {
			while (*p && *p != '^')
				p++;
			if (*p == '^')
				p++;
			cfg_r = s->scheme[ColFg].r / 65535.0;
			cfg_g = s->scheme[ColFg].g / 65535.0;
			cfg_b = s->scheme[ColFg].b / 65535.0;
			cbg_r = s->scheme[ColBg].r / 65535.0;
			cbg_g = s->scheme[ColBg].g / 65535.0;
			cbg_b = s->scheme[ColBg].b / 65535.0;
		} else {
			while (*p && *p != '^')
				p++;
			if (*p == '^')
				p++;
		}
		seg = p;
	}

	seg_len = (int) (p - seg);
	if (seg_len > 0 && (!render || (unsigned int) (cx - x) < w)) {
		if (seg_len >= (int) sizeof(buf))
			seg_len = (int) sizeof(buf) - 1;
		memcpy(buf, seg, (size_t) seg_len);
		buf[seg_len] = '\0';
		{
			PangoLayout          *layout;
			PangoFontDescription *desc;
			int                   tw, th;
			cairo_t              *tmp_cr;

			if (render) {
				tmp_cr = cr;
			} else {
				tmp_cr = cairo_create(s->cairo_surface);
			}

			layout = pango_cairo_create_layout(tmp_cr);
			if (g_plat.ui_dpi > 0.0)
				pango_cairo_context_set_resolution(
				    pango_layout_get_context(layout), g_plat.ui_dpi);
			desc = s->fonts ? s->fonts->desc : NULL;
			if (desc)
				pango_layout_set_font_description(layout, desc);
			pango_layout_set_text(layout, buf, -1);
			pango_layout_get_pixel_size(layout, &tw, &th);

			if (render) {
				cairo_set_source_rgb(cr, cbg_r, cbg_g, cbg_b);
				cairo_rectangle(
				    cr, cx, y, (double) (tw + g_plat.lrpad / 2), (double) h);
				cairo_fill(cr);
				cairo_set_source_rgb(cr, cfg_r, cfg_g, cfg_b);
				cairo_move_to(
				    cr, cx + g_plat.lrpad / 2, y + ((int) h - th) / 2);
				pango_cairo_show_layout(cr, layout);
			} else {
				cairo_destroy(tmp_cr);
			}
			g_object_unref(layout);

			cx += tw + g_plat.lrpad / 2;
			consumed += tw + g_plat.lrpad / 2;
		}
	}

	if (render)
		cairo_destroy(cr);
	return consumed;
}

/* ── map (blit to window) ───────────────────────────────────────────────── */

static void
render_map(AwmSurface *s, xcb_window_t win, int x, int y, unsigned int w,
    unsigned int h)
{
	if (!s)
		return;

	if (s->cairo_surface)
		cairo_surface_flush(s->cairo_surface);

	xcb_copy_area(s->xc, s->drawable, (xcb_drawable_t) win, s->gc, (int16_t) x,
	    (int16_t) y, (int16_t) x, (int16_t) y, (uint16_t) w, (uint16_t) h);
	xcb_flush(s->xc);
}

/* ── cursors ────────────────────────────────────────────────────────────── */

static Cur *
render_cur_create(AwmSurface *s, int shape)
{
	Cur *cur;

	if (!s || !(cur = ecalloc(1, sizeof(Cur))))
		return NULL;

	{
		xcb_font_t font = xcb_generate_id(s->xc);
		xcb_open_font(s->xc, font, strlen("cursor"), "cursor");
		cur->cursor = xcb_generate_id(s->xc);
		xcb_create_glyph_cursor(s->xc, cur->cursor, font, font,
		    (uint16_t) shape, (uint16_t) (shape + 1), 0, 0, 0, 65535, 65535,
		    65535);
		xcb_close_font(s->xc, font);
	}

	return cur;
}

static void
render_cur_free(AwmSurface *s, Cur *cursor)
{
	if (!cursor)
		return;

	xcb_free_cursor(s->xc, cursor->cursor);
	free(cursor);
}

/* ── vtable singleton ───────────────────────────────────────────────────── */

RenderBackend render_backend_cairo_xcb = {
	.create                 = render_create,
	.resize                 = render_resize,
	.free                   = render_free,
	.fontset_create         = render_fontset_create,
	.fontset_free           = render_fontset_free,
	.fontset_getwidth       = render_fontset_getwidth,
	.fontset_getwidth_clamp = render_fontset_getwidth_clamp,
	.clr_create             = render_clr_create,
	.scm_create             = render_scm_create,
	.cur_create             = render_cur_create,
	.cur_free               = render_cur_free,
	.setfontset             = render_setfontset,
	.setscheme              = render_setscheme,
	.rect                   = render_rect,
	.text                   = render_text,
	.pic                    = render_pic,
	.draw_statusd           = render_draw_statusd,
	.map                    = render_map,
};

/* ── opaque field accessors ─────────────────────────────────────────────── */

unsigned int
render_surface_font_height(const AwmSurface *s)
{
	if (!s || !s->fonts)
		return 0;
	return s->fonts->h;
}

xcb_visualtype_t *
render_surface_xcb_visual(const AwmSurface *s)
{
	if (!s)
		return NULL;
	return s->xcb_visual;
}

PangoFontDescription *
render_surface_font_desc(const AwmSurface *s)
{
	if (!s || !s->fonts)
		return NULL;
	return s->fonts->desc;
}
