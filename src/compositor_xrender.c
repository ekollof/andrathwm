/* compositor_xrender.c — XRender compositor backend for awm
 *
 * Implements the CompBackend vtable for the XRender fallback path.
 * Used on software-only X servers where EGL/KHR_image_pixmap is unavailable.
 *
 * Included in the build only when -DCOMPOSITOR is active.
 *
 * All private state (back-buffer, target picture, alpha picture cache) is
 * kept in the file-scope static `xr` struct.  Shared compositor state is
 * accessed through the `comp` extern defined in compositor.c.
 */

#ifdef COMPOSITOR

#include <xcb/xcb.h>
#include <xcb/render.h>
#include <xcb/xfixes.h>
#include <xcb/shape.h>
#include <xcb/xcb_renderutil.h>

#include "awm.h"
#include "log.h"
#include "compositor_backend.h"

/* -------------------------------------------------------------------------
 * Private backend state
 * ---------------------------------------------------------------------- */

static struct {
	xcb_render_picture_t target;      /* XRenderPicture on overlay          */
	xcb_pixmap_t         back_pixmap; /* off-screen back buffer pixmap      */
	xcb_render_picture_t back;        /* XRenderPicture on back_pixmap      */
	xcb_render_picture_t
	    alpha_pict[256]; /* pre-built 1×1 RepeatNormal solids */
	xcb_render_picture_t
	    wallpaper_pict; /* RepeatNormal picture on wallpaper */
} xr;

/* -------------------------------------------------------------------------
 * Helper: build a 1×1 A8 solid RepeatNormal picture for opacity masking
 * ---------------------------------------------------------------------- */

static xcb_render_picture_t
make_alpha_picture(double a)
{
	const xcb_render_pictforminfo_t *fi;
	xcb_pixmap_t                     pix;
	xcb_render_picture_t             pic;
	xcb_render_color_t               col;
	xcb_rectangle_t                  r    = { 0, 0, 1, 1 };
	uint32_t                         mask = XCB_RENDER_CP_REPEAT;
	uint32_t                         val  = XCB_RENDER_REPEAT_NORMAL;

	fi = xcb_render_util_find_standard_format(
	    comp.render_formats, XCB_PICT_STANDARD_A_8);
	pix = xcb_generate_id(xc);
	xcb_create_pixmap(xc, 8, pix, (xcb_drawable_t) root, 1, 1);
	pic = xcb_generate_id(xc);
	xcb_render_create_picture(
	    xc, pic, (xcb_drawable_t) pix, fi ? fi->id : 0, mask, &val);
	col.alpha = (uint16_t) (a * 0xffff);
	col.red = col.green = col.blue = 0;
	xcb_render_fill_rectangles(xc, XCB_RENDER_PICT_OP_SRC, pic, col, 1, &r);
	xcb_free_pixmap(xc, pix);
	return pic;
}

/* -------------------------------------------------------------------------
 * Backend vtable — init
 * ---------------------------------------------------------------------- */

static int
xrender_init(void)
{
	int                            i;
	const xcb_render_pictvisual_t *pv;
	xcb_render_pictformat_t        fmt;
	uint32_t                       pict_mask;
	uint32_t                       pict_val;

	pv = xcb_render_util_find_visual_format(
	    comp.render_formats, xcb_screen_root_visual(xc, screen));
	fmt = pv ? pv->format : 0;

	pict_mask = XCB_RENDER_CP_SUBWINDOW_MODE;
	pict_val  = XCB_SUBWINDOW_MODE_INCLUDE_INFERIORS;

	/* Overlay target picture */
	xr.target = xcb_generate_id(xc);
	xcb_render_create_picture(xr.target ? xc : xc, xr.target,
	    (xcb_drawable_t) comp.overlay, fmt, pict_mask, &pict_val);

	/* Back-buffer pixmap + picture */
	xr.back_pixmap = xcb_generate_id(xc);
	xcb_create_pixmap(xc, xcb_screen_root_depth(xc, screen), xr.back_pixmap,
	    (xcb_drawable_t) root, (uint16_t) sw, (uint16_t) sh);

	xr.back = xcb_generate_id(xc);
	xcb_render_create_picture(xc, xr.back, (xcb_drawable_t) xr.back_pixmap,
	    fmt, pict_mask, &pict_val);

	/* Alpha picture cache */
	for (i = 0; i < 256; i++)
		xr.alpha_pict[i] = make_alpha_picture((double) i / 255.0);

	xr.wallpaper_pict = 0;

	awm_debug("compositor/xrender: XRender fallback path initialised");
	return 0;
}

/* -------------------------------------------------------------------------
 * Backend vtable — cleanup
 * ---------------------------------------------------------------------- */

static void
xrender_cleanup(void)
{
	int i;

	if (xr.wallpaper_pict) {
		xcb_render_free_picture(xc, xr.wallpaper_pict);
		xr.wallpaper_pict = 0;
	}
	for (i = 0; i < 256; i++) {
		if (xr.alpha_pict[i])
			xcb_render_free_picture(xc, xr.alpha_pict[i]);
	}
	if (xr.back) {
		xcb_render_free_picture(xc, xr.back);
		xr.back = 0;
	}
	if (xr.back_pixmap) {
		xcb_free_pixmap(xc, xr.back_pixmap);
		xr.back_pixmap = 0;
	}
	if (xr.target) {
		xcb_render_free_picture(xc, xr.target);
		xr.target = 0;
	}
}

/* -------------------------------------------------------------------------
 * Backend vtable — bind / release pixmap
 * ---------------------------------------------------------------------- */

/* Apply the window's ShapeBounding clip region to cw->picture. */
static void
xrender_apply_shape(CompWin *cw)
{
	if (!cw->picture)
		return;

	if (!comp.has_xshape) {
		xcb_xfixes_set_picture_clip_region(xc, cw->picture, XCB_NONE, 0, 0);
		return;
	}

	{
		xcb_shape_get_rectangles_cookie_t sck;
		xcb_shape_get_rectangles_reply_t *sr;
		xcb_rectangle_t                  *rects;
		int                               nrects;

		sck = xcb_shape_get_rectangles(
		    xc, (xcb_window_t) cw->win, XCB_SHAPE_SK_BOUNDING);
		sr     = xcb_shape_get_rectangles_reply(xc, sck, NULL);
		rects  = sr ? xcb_shape_get_rectangles_rectangles(sr) : NULL;
		nrects = sr ? xcb_shape_get_rectangles_rectangles_length(sr) : 0;

		if (!rects || nrects == 0) {
			free(sr);
			xcb_xfixes_set_picture_clip_region(
			    xc, cw->picture, XCB_NONE, 0, 0);
			return;
		}

		{
			xcb_xfixes_region_t region = xcb_generate_id(xc);
			xcb_xfixes_create_region(xc, region, (uint32_t) nrects, rects);
			xcb_xfixes_set_picture_clip_region(xc, cw->picture, region, 0, 0);
			xcb_xfixes_destroy_region(xc, region);
		}
		free(sr);
	}
}

static void
xrender_bind_pixmap(CompWin *cw)
{
	const xcb_render_pictvisual_t *pv;
	xcb_render_pictformat_t        fmt;
	uint32_t                       pmask;
	uint32_t                       pval;
	xcb_void_cookie_t              ck;
	xcb_generic_error_t           *err;

	pv = xcb_render_util_find_visual_format(
	    comp.render_formats, xcb_screen_root_visual(xc, screen));
	fmt = pv ? pv->format : 0;
	if (cw->argb) {
		const xcb_render_pictforminfo_t *fi =
		    xcb_render_util_find_standard_format(
		        comp.render_formats, XCB_PICT_STANDARD_ARGB_32);
		fmt = fi ? fi->id : fmt;
	}
	pmask       = XCB_RENDER_CP_SUBWINDOW_MODE;
	pval        = XCB_SUBWINDOW_MODE_INCLUDE_INFERIORS;
	cw->picture = xcb_generate_id(xc);
	ck          = xcb_render_create_picture_checked(
        xc, cw->picture, (xcb_drawable_t) cw->pixmap, fmt, pmask, &pval);
	xcb_flush(xc);
	err = xcb_request_check(xc, ck);
	free(err); /* error intentionally discarded — pixmap may be gone */
	xrender_apply_shape(cw);
}

static void
xrender_release_pixmap(CompWin *cw)
{
	if (cw->picture) {
		xcb_render_free_picture(xc, cw->picture);
		cw->picture = 0;
	}
}

/* -------------------------------------------------------------------------
 * Backend vtable — wallpaper
 * ---------------------------------------------------------------------- */

static void
xrender_release_wallpaper(void)
{
	if (xr.wallpaper_pict) {
		xcb_render_free_picture(xc, xr.wallpaper_pict);
		xr.wallpaper_pict = 0;
	}
}

static void
xrender_update_wallpaper(void)
{
	const xcb_render_pictvisual_t *pv;
	xcb_render_pictformat_t        fmt;
	uint32_t                       pmask;
	uint32_t                       pval;
	xcb_void_cookie_t              ck;
	xcb_generic_error_t           *err;

	pv = xcb_render_util_find_visual_format(
	    comp.render_formats, xcb_screen_root_visual(xc, screen));
	fmt   = pv ? pv->format : 0;
	pmask = XCB_RENDER_CP_REPEAT;
	pval  = XCB_RENDER_REPEAT_NORMAL;

	xr.wallpaper_pict = xcb_generate_id(xc);
	ck = xcb_render_create_picture_checked(xc, xr.wallpaper_pict,
	    (xcb_drawable_t) comp.wallpaper_pixmap, fmt, pmask, &pval);
	xcb_flush(xc);
	err = xcb_request_check(xc, ck);
	if (err) {
		awm_warn("compositor/xrender: wallpaper picture creation failed "
		         "(error %d); background will be black",
		    (int) err->error_code);
		xcb_render_free_picture(xc, xr.wallpaper_pict);
		xr.wallpaper_pict = 0;
		free(err);
	}
}

/* -------------------------------------------------------------------------
 * Backend vtable — notify_resize
 * ---------------------------------------------------------------------- */

static void
xrender_notify_resize(void)
{
	const xcb_render_pictvisual_t *pv;
	xcb_render_pictformat_t        fmt;
	uint32_t                       pmask;
	uint32_t                       pval;

	if (xr.back) {
		xcb_render_free_picture(xc, xr.back);
		xr.back = 0;
	}
	if (xr.back_pixmap) {
		xcb_free_pixmap(xc, xr.back_pixmap);
		xr.back_pixmap = 0;
	}

	xr.back_pixmap = xcb_generate_id(xc);
	xcb_create_pixmap(xc, xcb_screen_root_depth(xc, screen), xr.back_pixmap,
	    (xcb_drawable_t) root, (uint16_t) sw, (uint16_t) sh);

	if (xr.back_pixmap) {
		pv = xcb_render_util_find_visual_format(
		    comp.render_formats, xcb_screen_root_visual(xc, screen));
		fmt     = pv ? pv->format : 0;
		pmask   = XCB_RENDER_CP_SUBWINDOW_MODE;
		pval    = XCB_SUBWINDOW_MODE_INCLUDE_INFERIORS;
		xr.back = xcb_generate_id(xc);
		xcb_render_create_picture(
		    xc, xr.back, (xcb_drawable_t) xr.back_pixmap, fmt, pmask, &pval);
	}
}

/* -------------------------------------------------------------------------
 * Backend vtable — repaint
 * ---------------------------------------------------------------------- */

static void
xrender_repaint(void)
{
	CompWin           *cw;
	xcb_render_color_t bg_color = { 0, 0, 0, 0xffff };

	xcb_xfixes_set_picture_clip_region(xc, xr.back, comp.dirty, 0, 0);

	if (xr.wallpaper_pict) {
		xcb_render_composite(xc, XCB_RENDER_PICT_OP_SRC, xr.wallpaper_pict,
		    XCB_NONE, xr.back, 0, 0, 0, 0, 0, 0, (uint16_t) sw, (uint16_t) sh);
	} else {
		xcb_rectangle_t bg_rect = { 0, 0, (uint16_t) sw, (uint16_t) sh };
		xcb_render_fill_rectangles(
		    xc, XCB_RENDER_PICT_OP_SRC, xr.back, bg_color, 1, &bg_rect);
	}

	for (cw = comp.windows; cw; cw = cw->next) {
		int                  alpha_idx;
		xcb_render_picture_t mask;

		if (!cw->redirected || cw->picture == 0 || cw->hidden)
			continue;

		alpha_idx = (int) (cw->opacity * 255.0 + 0.5);
		if (alpha_idx < 0)
			alpha_idx = 0;
		if (alpha_idx > 255)
			alpha_idx = 255;

		if (cw->argb || alpha_idx < 255) {
			mask = xr.alpha_pict[alpha_idx];
			xcb_render_composite(xc, XCB_RENDER_PICT_OP_OVER, cw->picture,
			    mask, xr.back, 0, 0, 0, 0, (int16_t) (cw->x + cw->bw),
			    (int16_t) (cw->y + cw->bw), (uint16_t) cw->w,
			    (uint16_t) cw->h);
		} else {
			xcb_render_composite(xc, XCB_RENDER_PICT_OP_SRC, cw->picture,
			    XCB_NONE, xr.back, 0, 0, 0, 0, (int16_t) (cw->x + cw->bw),
			    (int16_t) (cw->y + cw->bw), (uint16_t) cw->w,
			    (uint16_t) cw->h);
		}

		if (cw->client && cw->bw > 0) {
			int  sel = (selmon && cw->client == selmon->sel);
			Clr *clr = &scheme[sel ? SchemeSel : SchemeNorm][ColBorder];
			xcb_render_color_t bc         = { clr->r, clr->g, clr->b, clr->a };
			uint16_t           bw         = (uint16_t) cw->bw;
			uint16_t           ow         = (uint16_t) (cw->w + 2 * cw->bw);
			uint16_t           oh         = (uint16_t) (cw->h + 2 * cw->bw);
			xcb_rectangle_t    borders[4] = {
                { (int16_t) cw->x, (int16_t) cw->y, ow, bw },
                { (int16_t) cw->x, (int16_t) (cw->y + (int) (oh - bw)), ow,
				       bw },
                { (int16_t) cw->x, (int16_t) (cw->y + (int) bw), bw,
				       (uint16_t) cw->h },
                { (int16_t) (cw->x + (int) (ow - bw)),
				       (int16_t) (cw->y + (int) bw), bw, (uint16_t) cw->h },
			};
			xcb_render_fill_rectangles(
			    xc, XCB_RENDER_PICT_OP_SRC, xr.back, bc, 4, borders);
		}
	}

	/* Blit back-buffer to overlay — unconditional, no clip */
	xcb_xfixes_set_picture_clip_region(xc, xr.target, XCB_NONE, 0, 0);
	xcb_render_composite(xc, XCB_RENDER_PICT_OP_SRC, xr.back, XCB_NONE,
	    xr.target, 0, 0, 0, 0, 0, 0, (uint16_t) sw, (uint16_t) sh);

	comp_dirty_clear();
	xcb_xfixes_set_picture_clip_region(xc, xr.back, XCB_NONE, 0, 0);
	xflush();
}

/* -------------------------------------------------------------------------
 * Public accessor for comp_apply_shape — called from compositor.c when a
 * ShapeNotify arrives in the XRender path.
 * ---------------------------------------------------------------------- */

void
comp_xrender_apply_shape(CompWin *cw)
{
	xrender_apply_shape(cw);
}

/* -------------------------------------------------------------------------
 * Backend vtable singleton
 * ---------------------------------------------------------------------- */

const CompBackend comp_backend_xrender = {
	.init              = xrender_init,
	.cleanup           = xrender_cleanup,
	.bind_pixmap       = xrender_bind_pixmap,
	.release_pixmap    = xrender_release_pixmap,
	.update_wallpaper  = xrender_update_wallpaper,
	.release_wallpaper = xrender_release_wallpaper,
	.repaint           = xrender_repaint,
	.notify_resize     = xrender_notify_resize,
};

#endif /* COMPOSITOR */

/* Satisfy ISO C99: a translation unit must contain at least one declaration */
typedef int compositor_xrender_translation_unit_nonempty;
