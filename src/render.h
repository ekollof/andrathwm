/* See LICENSE file for copyright and license details. */

#ifndef RENDER_H
#define RENDER_H

#include <cairo/cairo.h>
#include <cairo/cairo-xcb.h>
#include <pango/pangocairo.h>
#include <xcb/xcb.h>
#include <xcb/render.h>
#include <xcb/xcb_renderutil.h>

/* -------------------------------------------------------------------------
 * Concrete drawing types (shared across render backends).
 * ---------------------------------------------------------------------- */

typedef struct {
	xcb_cursor_t cursor;
} Cur;

typedef struct Fnt {
	unsigned int          h;    /* line height in pixels (ascent + descent) */
	PangoFontDescription *desc; /* owned; freed in xfont_free() */
	struct Fnt           *next; /* kept for API compat; only head is used */
} Fnt;

enum { ColFg, ColBg, ColBorder }; /* Clr scheme index */
typedef struct {
	unsigned long  pixel;      /* X11 pixel value */
	unsigned short r, g, b, a; /* 16-bit channels */
} Clr;

/* AwmSurface — opaque draw context.
 * The X11/Cairo implementation (render_cairo_xcb.c) defines this as Drw. */
typedef struct Drw AwmSurface;

/* Keep Drw visible for callers that need the concrete struct fields
 * (sni.c, drw_map, etc.).  The full definition lives in render_cairo_xcb.c. */
typedef struct Drw Drw;

/* -------------------------------------------------------------------------
 * RenderBackend vtable
 * ---------------------------------------------------------------------- */

typedef struct {
	/* Lifecycle */
	AwmSurface *(*create)(xcb_connection_t *xc, int screen, xcb_window_t root,
	    unsigned int w, unsigned int h);
	void (*resize)(AwmSurface *s, unsigned int w, unsigned int h);
	void (*free)(AwmSurface *s);

	/* Font */
	Fnt *(*fontset_create)(AwmSurface *s, const char **fonts, size_t n);
	void (*fontset_free)(Fnt *set);
	unsigned int (*fontset_getwidth)(AwmSurface *s, const char *text);
	unsigned int (*fontset_getwidth_clamp)(
	    AwmSurface *s, const char *text, unsigned int n);

	/* Colour */
	void (*clr_create)(AwmSurface *s, Clr *dest, const char *hex);
	Clr *(*scm_create)(AwmSurface *s, char **names, size_t n);

	/* Cursor */
	Cur *(*cur_create)(AwmSurface *s, int shape);
	void (*cur_free)(AwmSurface *s, Cur *c);

	/* State */
	void (*setfontset)(AwmSurface *s, Fnt *set);
	void (*setscheme)(AwmSurface *s, Clr *scm);

	/* Drawing */
	void (*rect)(AwmSurface *s, int x, int y, unsigned int w, unsigned int h,
	    int filled, int invert);
	int (*text)(AwmSurface *s, int x, int y, unsigned int w, unsigned int h,
	    unsigned int lpad, const char *txt, int invert);
	void (*pic)(AwmSurface *s, int x, int y, unsigned int w, unsigned int h,
	    cairo_surface_t *icon);
	int (*draw_statusd)(AwmSurface *s, int x, int y, unsigned int w,
	    unsigned int h, const char *text);

	/* Flush to window */
	void (*map)(AwmSurface *s, xcb_window_t win, int x, int y, unsigned int w,
	    unsigned int h);
} RenderBackend;

extern RenderBackend *g_render_backend;

/* The single concrete render backend for X11+Cairo. */
extern RenderBackend render_backend_cairo_xcb;
extern RenderBackend render_backend_stub;

/* Accessors for opaque AwmSurface fields needed by callers that cannot
 * include the concrete struct definition (render_cairo_xcb.c only). */
unsigned int          render_surface_font_height(const AwmSurface *s);
xcb_visualtype_t     *render_surface_xcb_visual(const AwmSurface *s);
PangoFontDescription *render_surface_font_desc(const AwmSurface *s);

#endif /* RENDER_H */
