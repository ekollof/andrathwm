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

#include <stdlib.h>
#include <string.h>

#include <xcb/xcb.h>
#include <xcb/composite.h>
#include <xcb/render.h>
#include <xcb/xfixes.h>
#include <xcb/shape.h>
#include <xcb/xcb_renderutil.h>

#include <cairo/cairo.h>

#include "awm.h"
#include "wmstate.h"
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
	if (!fmt) {
		awm_warn("compositor/xrender: could not find picture format for "
		         "root visual");
		return -1;
	}

	pict_mask = XCB_RENDER_CP_SUBWINDOW_MODE;
	pict_val  = XCB_SUBWINDOW_MODE_INCLUDE_INFERIORS;

	/* Overlay target picture */
	xr.target = xcb_generate_id(xc);
	xcb_render_create_picture(xc, xr.target, (xcb_drawable_t) comp.overlay,
	    fmt, pict_mask, &pict_val);

	/* Back-buffer pixmap + picture */
	{
		xcb_void_cookie_t    ck;
		xcb_generic_error_t *perr;

		xr.back_pixmap = xcb_generate_id(xc);
		ck = xcb_create_pixmap_checked(xc, xcb_screen_root_depth(xc, screen),
		    xr.back_pixmap, (xcb_drawable_t) root, (uint16_t) sw,
		    (uint16_t) sh);
		xcb_flush(xc);
		perr = xcb_request_check(xc, ck);
		if (perr) {
			awm_warn("compositor/xrender: back-buffer pixmap creation "
			         "failed (error %d)",
			    (int) perr->error_code);
			free(perr);
			xr.back_pixmap = 0;
			return -1;
		}
	}

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

	/* Wallpaper is released by xrender_release_wallpaper() before cleanup()
	 * is called from compositor_cleanup().  The check below is a safety net
	 * only; in normal operation xr.wallpaper_pict is already 0 here. */
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
	if (err) {
		/* Picture creation failed — pixmap was likely destroyed between
		 * comp_refresh_pixmap and here.  Free the unused XID and bail out
		 * so subsequent ops don't operate on an invalid picture. */
		awm_warn("compositor/xrender: CreatePicture failed for window 0x%x "
		         "(error %d) — window will not be painted",
		    (unsigned) cw->win, (int) err->error_code);
		free(err);
		xcb_render_free_picture(xc, cw->picture);
		cw->picture = 0;
		return;
	}
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
	if (xr.back) {
		xcb_render_free_picture(xc, xr.back);
		xr.back = 0;
	}
	if (xr.back_pixmap) {
		xcb_free_pixmap(xc, xr.back_pixmap);
		xr.back_pixmap = 0;
	}

	{
		xcb_void_cookie_t              ck;
		xcb_generic_error_t           *perr;
		xcb_render_pictvisual_t const *pv2;
		xcb_render_pictformat_t        fmt2;
		uint32_t                       pmask2;
		uint32_t                       pval2;

		xr.back_pixmap = xcb_generate_id(xc);
		ck = xcb_create_pixmap_checked(xc, xcb_screen_root_depth(xc, screen),
		    xr.back_pixmap, (xcb_drawable_t) root, (uint16_t) sw,
		    (uint16_t) sh);
		xcb_flush(xc);
		perr = xcb_request_check(xc, ck);
		if (perr) {
			awm_warn("compositor/xrender: back-buffer resize pixmap "
			         "failed (error %d)",
			    (int) perr->error_code);
			free(perr);
			xr.back_pixmap = 0;
			return;
		}

		pv2 = xcb_render_util_find_visual_format(
		    comp.render_formats, xcb_screen_root_visual(xc, screen));
		fmt2    = pv2 ? pv2->format : 0;
		pmask2  = XCB_RENDER_CP_SUBWINDOW_MODE;
		pval2   = XCB_SUBWINDOW_MODE_INCLUDE_INFERIORS;
		xr.back = xcb_generate_id(xc);
		xcb_render_create_picture(xc, xr.back, (xcb_drawable_t) xr.back_pixmap,
		    fmt2, pmask2, &pval2);
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

	if (!xr.back)
		return;

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
			int sel =
			    (g_awm.selmon_num >= 0 && cw->client == g_awm_selmon->sel);
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

	/* Only clear dirty state if we are not paused.  If a fullscreen bypass
	 * raced in during rendering, leave dirty intact so the repaint loop
	 * restarts correctly when compositing resumes. */
	if (!comp.paused)
		comp_dirty_clear();
	xcb_xfixes_set_picture_clip_region(xc, xr.back, XCB_NONE, 0, 0);
	xflush();
}

/* -------------------------------------------------------------------------
 * Backend vtable — thumbnail capture
 * ---------------------------------------------------------------------- */

/* Helper: find XRender picture format for a given visual ID. */
static xcb_render_pictformat_t
xr_find_visual_format(xcb_visualid_t vid)
{
	const xcb_render_pictvisual_t *pv;

	if (!comp.render_formats)
		return 0;
	pv = xcb_render_util_find_visual_format(comp.render_formats, vid);
	return pv ? pv->format : 0;
}

/* Helper: find XRender picture format for a given depth. */
static xcb_render_pictformat_t
xr_find_format_for_depth(int depth)
{
	const xcb_render_pictforminfo_t *fi;

	if (!comp.render_formats)
		return 0;
	if (depth == 32) {
		fi = xcb_render_util_find_standard_format(
		    comp.render_formats, XCB_PICT_STANDARD_ARGB_32);
		if (fi)
			return fi->id;
	}
	if (depth == 24) {
		fi = xcb_render_util_find_standard_format(
		    comp.render_formats, XCB_PICT_STANDARD_RGB_24);
		if (fi)
			return fi->id;
	}
	return xr_find_visual_format(xcb_screen_root_visual(xc, screen));
}

/* Capture a scaled thumbnail from a window via XRender + xcb_get_image. */
static cairo_surface_t *
xrender_capture_thumb(CompWin *cw, int max_w, int max_h)
{
	double                  sx, sy, scale;
	int                     tw, th;
	xcb_render_pictformat_t src_fmt, dst_fmt;
	xcb_render_picture_t    src_pict = 0, dst_pict = 0;
	xcb_pixmap_t            dst_pixmap = 0;
	xcb_pixmap_t            own_pixmap = 0;
	xcb_pixmap_t            src_pixmap;
	xcb_render_transform_t  xform;
	xcb_get_image_cookie_t  gck;
	xcb_get_image_reply_t  *gr       = NULL;
	cairo_surface_t        *surf     = NULL;
	uint8_t                *img_data = NULL;

	if (!cw || cw->w <= 0 || cw->h <= 0)
		return NULL;

	/* Compute thumbnail size preserving aspect ratio */
	sx    = (double) max_w / (double) cw->w;
	sy    = (double) max_h / (double) cw->h;
	scale = sx < sy ? sx : sy;
	if (scale > 1.0)
		scale = 1.0;
	tw = (int) (cw->w * scale);
	th = (int) (cw->h * scale);
	if (tw < 1)
		tw = 1;
	if (th < 1)
		th = 1;

	/* Source pixmap: prefer cw->pixmap, fall back to a fresh acquire */
	src_pixmap = cw->pixmap;
	if (!src_pixmap) {
		xcb_pixmap_t         pix = xcb_generate_id(xc);
		xcb_void_cookie_t    nck;
		xcb_generic_error_t *nerr;
		nck = xcb_composite_name_window_pixmap_checked(
		    xc, (xcb_window_t) cw->win, pix);
		xcb_flush(xc);
		nerr = xcb_request_check(xc, nck);
		if (nerr) {
			free(nerr);
			return NULL;
		}
		own_pixmap = pix;
		src_pixmap = pix;
	}

	/* Source picture with scale transform */
	src_fmt = xr_find_format_for_depth(cw->depth);
	if (!src_fmt)
		goto out;

	src_pict = xcb_generate_id(xc);
	{
		uint32_t pmask = 0, pval = 0;
		xcb_render_create_picture(
		    xc, src_pict, (xcb_drawable_t) src_pixmap, src_fmt, pmask, &pval);
	}

	{
		double             inv    = 1.0 / scale;
		xcb_render_fixed_t fp_inv = (xcb_render_fixed_t) (inv * 65536.0 + 0.5);
		xcb_render_fixed_t fp_one = 65536;

		xform.matrix11 = fp_inv;
		xform.matrix12 = 0;
		xform.matrix13 = 0;
		xform.matrix21 = 0;
		xform.matrix22 = fp_inv;
		xform.matrix23 = 0;
		xform.matrix31 = 0;
		xform.matrix32 = 0;
		xform.matrix33 = fp_one;
	}
	xcb_render_set_picture_transform(xc, src_pict, xform);
	{
		static const char filter[] = "good";
		xcb_render_set_picture_filter(
		    xc, src_pict, (uint16_t) (sizeof(filter) - 1), filter, 0, NULL);
	}

	/* Destination pixmap at thumbnail size (root depth) */
	dst_fmt = xr_find_visual_format(xcb_screen_root_visual(xc, screen));
	if (!dst_fmt)
		goto out;

	dst_pixmap = xcb_generate_id(xc);
	{
		uint8_t dst_depth = (uint8_t) xcb_screen_root_depth(xc, screen);
		xcb_void_cookie_t    ck;
		xcb_generic_error_t *err;
		ck = xcb_create_pixmap_checked(xc, dst_depth, dst_pixmap,
		    (xcb_drawable_t) root, (uint16_t) tw, (uint16_t) th);
		xcb_flush(xc);
		err = xcb_request_check(xc, ck);
		if (err) {
			free(err);
			dst_pixmap = 0;
			goto out;
		}
	}

	dst_pict = xcb_generate_id(xc);
	{
		uint32_t pmask = 0, pval = 0;
		xcb_render_create_picture(
		    xc, dst_pict, (xcb_drawable_t) dst_pixmap, dst_fmt, pmask, &pval);
	}

	/* Scale-composite source → destination */
	xcb_render_composite(xc, XCB_RENDER_PICT_OP_SRC, src_pict,
	    XCB_RENDER_PICTURE_NONE, dst_pict, 0, 0, 0, 0, 0, 0, (uint16_t) tw,
	    (uint16_t) th);
	xcb_flush(xc);

	/* Read the pixels back */
	gck = xcb_get_image(xc, XCB_IMAGE_FORMAT_Z_PIXMAP,
	    (xcb_drawable_t) dst_pixmap, 0, 0, (uint16_t) tw, (uint16_t) th,
	    0xffffffff);
	gr  = xcb_get_image_reply(xc, gck, NULL);
	if (!gr)
		goto out;

	{
		int      data_len = xcb_get_image_data_length(gr);
		uint8_t *data     = xcb_get_image_data(gr);
		int      stride   = tw * 4;

		if (data_len < stride * th)
			goto out;

		img_data = malloc((size_t) (stride * th));
		if (!img_data)
			goto out;
		memcpy(img_data, data, (size_t) (stride * th));

		surf = cairo_image_surface_create_for_data(
		    img_data, CAIRO_FORMAT_RGB24, tw, th, stride);
		if (!surf || cairo_surface_status(surf) != CAIRO_STATUS_SUCCESS) {
			if (surf) {
				cairo_surface_destroy(surf);
				surf = NULL;
			}
			free(img_data);
			img_data = NULL;
		} else {
			/* Transfer ownership of img_data to the surface */
			cairo_surface_set_user_data(
			    surf, (const cairo_user_data_key_t *) &comp, img_data, free);
			img_data = NULL;
		}
	}

out:
	if (src_pict)
		xcb_render_free_picture(xc, src_pict);
	if (dst_pict)
		xcb_render_free_picture(xc, dst_pict);
	if (dst_pixmap)
		xcb_free_pixmap(xc, dst_pixmap);
	if (own_pixmap)
		xcb_free_pixmap(xc, own_pixmap);
	free(gr);
	free(img_data);
	return surf;
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
	.capture_thumb     = xrender_capture_thumb,
	.apply_shape       = xrender_apply_shape,
	.notify_damage = NULL, /* XRender Pictures track pixmap contents live */
};

#endif /* COMPOSITOR */

/* Satisfy ISO C99: a translation unit must contain at least one declaration */
typedef int compositor_xrender_translation_unit_nonempty;
