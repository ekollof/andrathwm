/* See LICENSE file for copyright and license details. */

#ifndef DRW_H
#define DRW_H

#include <cairo/cairo.h>
#include <cairo/cairo-xcb.h>
#include <pango/pangocairo.h>
#include <xcb/xcb.h>
#include <xcb/render.h>
#include <xcb/xcb_renderutil.h>

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
	unsigned long  pixel; /* X11 pixel value — used by drw_rect/drw_text */
	unsigned short r, g, b, a; /* 16-bit channels — used by clr_to_argb() */
} Clr;

typedef struct {
	unsigned int      w, h;
	xcb_connection_t *xc; /* main XCB connection (shared, not owned) */
	int               screen;
	xcb_window_t      root;
	xcb_pixmap_t      drawable;
	xcb_gcontext_t    gc;
	Clr              *scheme;
	Fnt              *fonts;
	xcb_connection_t
	    *cairo_xcb; /* dedicated XCB conn for cairo — never read by Xlib */
	xcb_visualtype_t *xcb_visual;    /* matches root visual for screen */
	cairo_surface_t  *cairo_surface; /* cached surface for icon rendering */
} Drw;

/* Drawable abstraction */
Drw *drw_create(xcb_connection_t *xc, int screen, xcb_window_t root,
    unsigned int w, unsigned int h);
void drw_resize(Drw *drw, unsigned int w, unsigned int h);
void drw_free(Drw *drw);

/* Fnt abstraction */
Fnt *drw_fontset_create(Drw *drw, const char *fonts[], size_t fontcount);
void drw_fontset_free(Fnt *set);
unsigned int drw_fontset_getwidth(Drw *drw, const char *text);
unsigned int drw_fontset_getwidth_clamp(
    Drw *drw, const char *text, unsigned int n);

/* Colorscheme abstraction */
void drw_clr_create(Drw *drw, Clr *dest, const char *clrname);
Clr *drw_scm_create(Drw *drw, char *clrnames[], size_t clrcount);

/* Cursor abstraction */
Cur *drw_cur_create(Drw *drw, int shape);
void drw_cur_free(Drw *drw, Cur *cursor);

/* Drawing context manipulation */
void drw_setfontset(Drw *drw, Fnt *set);
void drw_setscheme(Drw *drw, Clr *scm);

/* Drawing functions */
void drw_rect(Drw *drw, int x, int y, unsigned int w, unsigned int h,
    int filled, int invert);
int  drw_text(Drw *drw, int x, int y, unsigned int w, unsigned int h,
     unsigned int lpad, const char *text, int invert);
void drw_pic(Drw *drw, int x, int y, unsigned int w, unsigned int h,
    cairo_surface_t *surface);

/* Map functions */
void drw_map(
    Drw *drw, xcb_window_t win, int x, int y, unsigned int w, unsigned int h);

#endif /* DRW_H */
