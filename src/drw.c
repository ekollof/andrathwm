/* See LICENSE file for copyright and license details. */
#include <X11/extensions/Xrender.h>
#include <stdlib.h>
#include <string.h>
#include <xcb/xcb.h>
#include <xcb/xproto.h>

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

#define UTF_INVALID 0xFFFD
#define UTF_SIZ 4

static const unsigned char utfbyte[UTF_SIZ + 1] = { 0x80, 0, 0xC0, 0xE0,
	0xF0 };
static const unsigned char utfmask[UTF_SIZ + 1] = { 0xC0, 0x80, 0xE0, 0xF0,
	0xF8 };
static const long utfmin[UTF_SIZ + 1] = { 0, 0, 0x80, 0x800, 0x10000 };
static const long utfmax[UTF_SIZ + 1] = { 0x10FFFF, 0x7F, 0x7FF, 0xFFFF,
	0x10FFFF };

static long
utf8decodebyte(const char c, size_t *i)
{
	for (*i = 0; *i < (UTF_SIZ + 1); ++(*i))
		if (((unsigned char) c & utfmask[*i]) == utfbyte[*i])
			return (unsigned char) c & ~utfmask[*i];
	return 0;
}

static size_t
utf8validate(long *u, size_t i)
{
	if (!BETWEEN(*u, utfmin[i], utfmax[i]) || BETWEEN(*u, 0xD800, 0xDFFF))
		*u = UTF_INVALID;
	for (i = 1; *u > utfmax[i]; ++i)
		;
	return i;
}

static size_t
utf8decode(const char *c, long *u, size_t clen)
{
	size_t i, j, len, type;
	long   udecoded;

	*u = UTF_INVALID;
	if (!clen)
		return 0;
	udecoded = utf8decodebyte(c[0], &len);
	if (!BETWEEN(len, 1, UTF_SIZ))
		return 1;
	for (i = 1, j = 1; i < clen && j < len; ++i, ++j) {
		udecoded = (udecoded << 6) | utf8decodebyte(c[i], &type);
		if (type)
			return j;
	}
	if (j < len)
		return 0;
	*u = udecoded;
	utf8validate(u, len);

	return len;
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
		xcb_visualid_t vid = XVisualIDFromVisual(DefaultVisual(dpy, screen));
		drw->xcb_visual    = xcb_find_visualtype(drw->cairo_xcb, screen, vid);
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
xfont_create(Drw *drw, const char *fontname, FcPattern *fontpattern)
{
	Fnt       *font;
	XftFont   *xfont   = NULL;
	FcPattern *pattern = NULL;

	if (fontname) {
		/* Using the pattern found at font->xfont->pattern does not yield the
		 * same substitution results as using the pattern returned by
		 * FcNameParse; using the latter results in the desired fallback
		 * behaviour whereas the former just results in missing-character
		 * rectangles being drawn, at least with some fonts. */
		if (!(xfont = XftFontOpenName(drw->dpy, drw->screen, fontname))) {
			awm_error("cannot load font from name: '%s'", fontname);
			return NULL;
		}
		if (!(pattern = FcNameParse((FcChar8 *) fontname))) {
			awm_error("cannot parse font name to pattern: '%s'", fontname);
			XftFontClose(drw->dpy, xfont);
			return NULL;
		}
	} else if (fontpattern) {
		if (!(xfont = XftFontOpenPattern(drw->dpy, fontpattern))) {
			awm_error("cannot load font from pattern");
			return NULL;
		}
	} else {
		die("no font specified.");
	}

	font          = ecalloc(1, sizeof(Fnt));
	font->xfont   = xfont;
	font->pattern = pattern;
	font->h       = xfont->ascent + xfont->descent;
	font->dpy     = drw->dpy;

	return font;
}

static void
xfont_free(Fnt *font)
{
	if (!font)
		return;
	if (font->pattern)
		FcPatternDestroy(font->pattern);
	XftFontClose(font->dpy, font->xfont);
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
		if ((cur = xfont_create(drw, fonts[fontcount - i], NULL))) {
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
	if (!drw || !dest || !clrname)
		return;

	if (!XftColorAllocName(drw->dpy, DefaultVisual(drw->dpy, drw->screen),
	        DefaultColormap(drw->dpy, drw->screen), clrname, dest))
		die("error, cannot allocate color '%s'", clrname);
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
	    !(ret = ecalloc(clrcount, sizeof(XftColor))))
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
	int          i, ty, ellipsis_x    = 0;
	unsigned int tmpw, ew, ellipsis_w = 0, ellipsis_len;
	XftDraw     *d = NULL;
	Fnt         *usedfont, *curfont, *nextfont;
	int          utf8strlen, utf8charlen, render = x || y || w || h;
	long         utf8codepoint = 0;
	const char  *utf8str;
	FcCharSet   *fccharset;
	FcPattern   *fcpattern;
	FcPattern   *match;
	XftResult    result;
	int          charexists = 0, overflow = 0;
	/* keep track of a couple codepoints for which we have no match. */
	enum { nomatches_len = 64 };
	static struct {
		long         codepoint[nomatches_len];
		unsigned int idx;
	} nomatches;
	static unsigned int ellipsis_width = 0;

	if (!drw || (render && (!drw->scheme || !w)) || !text || !drw->fonts)
		return 0;

	if (!render) {
		w = invert ? invert : ~invert;
	} else {
		XSetForeground(
		    drw->dpy, drw->gc, drw->scheme[invert ? ColFg : ColBg].pixel);
		XFillRectangle(drw->dpy, drw->drawable, drw->gc, x, y, w, h);
		d = XftDrawCreate(drw->dpy, drw->drawable,
		    DefaultVisual(drw->dpy, drw->screen),
		    DefaultColormap(drw->dpy, drw->screen));
		x += lpad;
		w -= lpad;
	}

	usedfont = drw->fonts;
	if (!ellipsis_width && render)
		ellipsis_width = drw_fontset_getwidth(drw, "...");
	while (1) {
		ew = ellipsis_len = utf8strlen = 0;
		utf8str                        = text;
		nextfont                       = NULL;
		while (*text) {
			utf8charlen = utf8decode(text, &utf8codepoint, UTF_SIZ);
			for (curfont = drw->fonts; curfont; curfont = curfont->next) {
				charexists = charexists ||
				    XftCharExists(drw->dpy, curfont->xfont, utf8codepoint);
				if (charexists) {
					drw_font_getexts(curfont, text, utf8charlen, &tmpw, NULL);
					if (ew + ellipsis_width <= w) {
						/* keep track where the ellipsis still fits */
						ellipsis_x   = x + ew;
						ellipsis_w   = w - ew;
						ellipsis_len = utf8strlen;
					}

					if (ew + tmpw > w) {
						overflow = 1;
						/* called from drw_fontset_getwidth_clamp():
						 * it wants the width AFTER the overflow
						 */
						if (!render)
							x += tmpw;
						else
							utf8strlen = ellipsis_len;
					} else if (curfont == usedfont) {
						utf8strlen += utf8charlen;
						text += utf8charlen;
						ew += tmpw;
					} else {
						nextfont = curfont;
					}
					break;
				}
			}

			if (overflow || !charexists || nextfont)
				break;
			else
				charexists = 0;
		}

		if (utf8strlen) {
			if (render) {
				ty = y + (h - usedfont->h) / 2 + usedfont->xfont->ascent;
				XftDrawStringUtf8(d, &drw->scheme[invert ? ColBg : ColFg],
				    usedfont->xfont, x, ty, (XftChar8 *) utf8str, utf8strlen);
			}
			x += ew;
			w -= ew;
		}
		if (render && overflow)
			drw_text(drw, ellipsis_x, y, ellipsis_w, h, 0, "...", invert);

		if (!*text || overflow) {
			break;
		} else if (nextfont) {
			charexists = 0;
			usedfont   = nextfont;
		} else {
			/* Regardless of whether or not a fallback font is found, the
			 * character must be drawn. */
			charexists = 1;

			for (i = 0; i < nomatches_len; ++i) {
				/* avoid calling XftFontMatch if we know we won't find a match
				 */
				if (utf8codepoint == nomatches.codepoint[i])
					goto no_match;
			}

			fccharset = FcCharSetCreate();
			FcCharSetAddChar(fccharset, utf8codepoint);

			if (!drw->fonts->pattern) {
				/* Refer to the comment in xfont_create for more information.
				 */
				die("the first font in the cache must be loaded from a font "
				    "string.");
			}

			fcpattern = FcPatternDuplicate(drw->fonts->pattern);
			FcPatternAddCharSet(fcpattern, FC_CHARSET, fccharset);
			FcPatternAddBool(fcpattern, FC_SCALABLE, FcTrue);

			FcConfigSubstitute(NULL, fcpattern, FcMatchPattern);
			FcDefaultSubstitute(fcpattern);
			match = XftFontMatch(drw->dpy, drw->screen, fcpattern, &result);

			FcCharSetDestroy(fccharset);
			FcPatternDestroy(fcpattern);

			if (match) {
				usedfont = xfont_create(drw, NULL, match);
				if (usedfont &&
				    XftCharExists(drw->dpy, usedfont->xfont, utf8codepoint)) {
					for (curfont = drw->fonts; curfont->next;
					     curfont = curfont->next)
						; /* NOP */
					curfont->next = usedfont;
				} else {
					xfont_free(usedfont);
					nomatches.codepoint[++nomatches.idx % nomatches_len] =
					    utf8codepoint;
				no_match:
					usedfont = drw->fonts;
				}
			}
		}
	}
	if (d)
		XftDrawDestroy(d);

	return x + (render ? w : 0);
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
	XSync(drw->dpy, False);
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

void
drw_font_getexts(Fnt *font, const char *text, unsigned int len,
    unsigned int *w, unsigned int *h)
{
	XGlyphInfo ext;

	if (!font || !text)
		return;

	XftTextExtentsUtf8(font->dpy, font->xfont, (XftChar8 *) text, len, &ext);
	if (w)
		*w = ext.xOff;
	if (h)
		*h = font->h;
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
