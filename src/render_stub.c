/* See LICENSE file for copyright and license details. */

#include <stdlib.h>
#include <string.h>

#include "render.h"

struct Drw {
	unsigned int font_h;
};

RenderBackend *g_render_backend = NULL;

static AwmSurface *
stub_create(xcb_connection_t *xc, int screen, xcb_window_t root,
    unsigned int w, unsigned int h)
{
	AwmSurface *s;

	(void) xc;
	(void) screen;
	(void) root;
	(void) w;
	(void) h;
	s = calloc(1, sizeof(AwmSurface));
	if (s)
		s->font_h = 12;
	return s;
}

static void
stub_resize(AwmSurface *s, unsigned int w, unsigned int h)
{
	(void) s;
	(void) w;
	(void) h;
}

static void
stub_free(AwmSurface *s)
{
	free(s);
}

static Fnt *
stub_fontset_create(AwmSurface *s, const char **fonts, size_t n)
{
	Fnt *f;

	(void) fonts;
	(void) n;
	if (s)
		s->font_h = 12;
	f = calloc(1, sizeof(Fnt));
	if (f)
		f->h = 12;
	return f;
}

static void
stub_fontset_free(Fnt *set)
{
	free(set);
}

static unsigned int
stub_fontset_getwidth(AwmSurface *s, const char *text)
{
	(void) s;
	return text ? (unsigned int) (strlen(text) * 8) : 0;
}

static unsigned int
stub_fontset_getwidth_clamp(AwmSurface *s, const char *text, unsigned int n)
{
	unsigned int w;

	(void) s;
	if (!text)
		return 0;
	w = (unsigned int) strlen(text) * 8;
	return w > n ? n : w;
}

static void
stub_clr_create(AwmSurface *s, Clr *dest, const char *hex)
{
	(void) s;
	(void) hex;
	if (dest)
		memset(dest, 0, sizeof(*dest));
}

static Clr *
stub_scm_create(AwmSurface *s, char **names, size_t n)
{
	Clr *arr;

	(void) s;
	(void) names;
	arr = calloc(n ? n : 1, sizeof(Clr));
	return arr;
}

static Cur *
stub_cur_create(AwmSurface *s, int shape)
{
	Cur *c;

	(void) s;
	(void) shape;
	c = calloc(1, sizeof(Cur));
	return c;
}

static void
stub_cur_free(AwmSurface *s, Cur *c)
{
	(void) s;
	free(c);
}

static void
stub_setfontset(AwmSurface *s, Fnt *set)
{
	(void) s;
	(void) set;
}

static void
stub_setscheme(AwmSurface *s, Clr *scm)
{
	(void) s;
	(void) scm;
}

static void
stub_rect(AwmSurface *s, int x, int y, unsigned int w, unsigned int h,
    int filled, int invert)
{
	(void) s;
	(void) x;
	(void) y;
	(void) w;
	(void) h;
	(void) filled;
	(void) invert;
}

static int
stub_text(AwmSurface *s, int x, int y, unsigned int w, unsigned int h,
    unsigned int lpad, const char *txt, int invert)
{
	(void) s;
	(void) y;
	(void) h;
	(void) lpad;
	(void) txt;
	(void) invert;
	return x + (int) w;
}

static void
stub_pic(AwmSurface *s, int x, int y, unsigned int w, unsigned int h,
    cairo_surface_t *icon)
{
	(void) s;
	(void) x;
	(void) y;
	(void) w;
	(void) h;
	(void) icon;
}

static int
stub_draw_statusd(
    AwmSurface *s, int x, int y, unsigned int w, unsigned int h, const char *t)
{
	(void) s;
	(void) y;
	(void) h;
	(void) t;
	return x + (int) w;
}

static void
stub_map(AwmSurface *s, xcb_window_t win, int x, int y, unsigned int w,
    unsigned int h)
{
	(void) s;
	(void) win;
	(void) x;
	(void) y;
	(void) w;
	(void) h;
}

RenderBackend render_backend_stub = {
	.create                 = stub_create,
	.resize                 = stub_resize,
	.free                   = stub_free,
	.fontset_create         = stub_fontset_create,
	.fontset_free           = stub_fontset_free,
	.fontset_getwidth       = stub_fontset_getwidth,
	.fontset_getwidth_clamp = stub_fontset_getwidth_clamp,
	.clr_create             = stub_clr_create,
	.scm_create             = stub_scm_create,
	.cur_create             = stub_cur_create,
	.cur_free               = stub_cur_free,
	.setfontset             = stub_setfontset,
	.setscheme              = stub_setscheme,
	.rect                   = stub_rect,
	.text                   = stub_text,
	.pic                    = stub_pic,
	.draw_statusd           = stub_draw_statusd,
	.map                    = stub_map,
};

unsigned int
render_surface_font_height(const AwmSurface *s)
{
	return s ? s->font_h : 12;
}

xcb_visualtype_t *
render_surface_xcb_visual(const AwmSurface *s)
{
	(void) s;
	return NULL;
}

PangoFontDescription *
render_surface_font_desc(const AwmSurface *s)
{
	(void) s;
	return NULL;
}
