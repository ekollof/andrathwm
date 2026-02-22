/* compositor.c — compositor core for awm
 *
 * Architecture:
 *   - XCompositeRedirectSubwindows(root, CompositeRedirectManual) captures
 *     all root children into server-side pixmaps.
 *   - An overlay window (XCompositeGetOverlayWindow) is used as the render
 *     surface.  The active backend (EGL/GL or XRender) draws into it.
 *   - Each window's XCompositeNameWindowPixmap is handed to the backend
 *     via bind_pixmap() so the backend can build whatever resource it needs
 *     (EGLImageKHR+texture for the GL path; XRenderPicture for XRender).
 *   - XDamage tracks which windows have changed since the last repaint.
 *   - Border rectangles for managed clients are drawn by the active backend
 *     in the same repaint pass.
 *
 * Backend selection:
 *   EGL/GL (comp_backend_egl) is tried first.  If EGL_KHR_image_pixmap is
 *   unavailable the compositor falls back to XRender (comp_backend_xrender)
 *   so the WM still works on software-only X servers.
 *
 * Compile-time guard: the entire file is dead code unless -DCOMPOSITOR.
 */

#ifdef COMPOSITOR

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include <xcb/xcb.h>
#include <xcb/composite.h>
#include <xcb/damage.h>
#include <xcb/render.h>
#include <xcb/xfixes.h>
#include <xcb/shape.h>
#include <xcb/xcb_renderutil.h>
#include <xcb/present.h>

#include <glib.h>

#include "awm.h"
#include "log.h"
#include "compositor.h"
#include "compositor_backend.h"

/* -------------------------------------------------------------------------
 * Shared compositor state — single instance, accessed by all backend files
 * ---------------------------------------------------------------------- */

CompShared comp;

/* ---- compositor compile-time invariants ---- */
_Static_assert(sizeof(unsigned short) == 2,
    "unsigned short must be 16 bits for xcb_render_color_t alpha/channel "
    "field scaling");
_Static_assert(sizeof(short) == 2,
    "short must be 16 bits to match xcb_rectangle_t x and y field types");
_Static_assert(sizeof(xcb_pixmap_t) == sizeof(uint32_t),
    "xcb_pixmap_t must be 32 bits for format-32 property reads");

/* -------------------------------------------------------------------------
 * Forward declarations (internal)
 * ---------------------------------------------------------------------- */

static void     comp_add_by_xid(xcb_window_t w);
static CompWin *comp_find_by_xid(xcb_window_t w);
static CompWin *comp_find_by_client(Client *c);
static void     comp_free_win(CompWin *cw);
static void     comp_refresh_pixmap(CompWin *cw);
static void     comp_update_wallpaper(void);
static void     schedule_repaint(void);
static void     comp_do_repaint(void);
static void     comp_arm_vblank(void);
static gboolean comp_repaint_idle(gpointer data);

/* -------------------------------------------------------------------------
 * CPU-side dirty bbox helpers
 *
 * comp_dirty_add_rect(x,y,w,h)  — union a rectangle into comp.dirty (server)
 *                                  and extend the CPU bbox.
 * comp_dirty_full()              — mark the whole screen dirty.
 * comp_dirty_clear()             — reset to empty after a repaint.
 *                                  (defined as static inline in
 * compositor_backend.h so it is also visible in the backend files)
 * ---------------------------------------------------------------------- */

static void
comp_dirty_add_rect(int x, int y, int w, int h)
{
	xcb_rectangle_t     r;
	xcb_xfixes_region_t sr;

	if (w <= 0 || h <= 0)
		return;

	r.x      = (short) x;
	r.y      = (short) y;
	r.width  = (uint16_t) w;
	r.height = (uint16_t) h;
	sr       = xcb_generate_id(xc);
	xcb_xfixes_create_region(xc, sr, 1, &r);
	xcb_xfixes_union_region(xc, comp.dirty, sr, comp.dirty);
	xcb_xfixes_destroy_region(xc, sr);

	if (!comp.dirty_bbox_valid) {
		comp.dirty_x1         = x;
		comp.dirty_y1         = y;
		comp.dirty_x2         = x + w;
		comp.dirty_y2         = y + h;
		comp.dirty_bbox_valid = 1;
	} else {
		if (x < comp.dirty_x1)
			comp.dirty_x1 = x;
		if (y < comp.dirty_y1)
			comp.dirty_y1 = y;
		if (x + w > comp.dirty_x2)
			comp.dirty_x2 = x + w;
		if (y + h > comp.dirty_y2)
			comp.dirty_y2 = y + h;
	}
}

static void
comp_dirty_full(void)
{
	xcb_rectangle_t full = { 0, 0, (uint16_t) sw, (uint16_t) sh };
	xcb_xfixes_set_region(xc, comp.dirty, 1, &full);
	comp.dirty_x1         = 0;
	comp.dirty_y1         = 0;
	comp.dirty_x2         = sw;
	comp.dirty_y2         = sh;
	comp.dirty_bbox_valid = 1;
}

/* Declared in compositor_xrender.c; called from the ShapeNotify handler. */
void comp_xrender_apply_shape(CompWin *cw);

/* -------------------------------------------------------------------------
 * compositor_init
 * ---------------------------------------------------------------------- */

int
compositor_init(GMainContext *ctx)
{
	int                                i;
	const xcb_query_extension_reply_t *ext;

	memset(&comp, 0, sizeof(comp));
	comp.ctx = ctx;

	/* --- Query/cache XRender picture formats (needed for all format lookups)
	 */
	comp.render_formats = xcb_render_util_query_formats(xc);
	if (!comp.render_formats) {
		awm_warn("compositor: xcb_render_util_query_formats failed");
		return -1;
	}

	/* --- Check required extensions ----------------------------------------
	 */

	ext = xcb_get_extension_data(xc, &xcb_composite_id);
	if (!ext || !ext->present) {
		awm_warn("compositor: XComposite extension not available");
		return -1;
	}
	{
		xcb_composite_query_version_cookie_t vck;
		xcb_composite_query_version_reply_t *vr;
		vck = xcb_composite_query_version(xc, 0, 2);
		vr  = xcb_composite_query_version_reply(xc, vck, NULL);
		if (!vr || (vr->major_version == 0 && vr->minor_version < 2)) {
			awm_warn("compositor: XComposite >= 0.2 required (got %d.%d)",
			    vr ? (int) vr->major_version : 0,
			    vr ? (int) vr->minor_version : 0);
			free(vr);
			return -1;
		}
		free(vr);
	}

	ext = xcb_get_extension_data(xc, &xcb_damage_id);
	if (!ext || !ext->present) {
		awm_warn("compositor: XDamage extension not available");
		return -1;
	}
	comp.damage_ev_base  = ext->first_event;
	comp.damage_err_base = ext->first_error;
	comp.damage_req_base = ext->major_opcode;
	{
		xcb_damage_query_version_cookie_t dvck;
		xcb_damage_query_version_reply_t *dvr;
		dvck = xcb_damage_query_version(xc, 1, 1);
		dvr  = xcb_damage_query_version_reply(xc, dvck, NULL);
		free(dvr);
	}

	ext = xcb_get_extension_data(xc, &xcb_xfixes_id);
	if (!ext || !ext->present) {
		awm_warn("compositor: XFixes extension not available");
		return -1;
	}
	comp.xfixes_ev_base  = ext->first_event;
	comp.xfixes_err_base = ext->first_error;
	{
		xcb_xfixes_query_version_cookie_t fvck;
		xcb_xfixes_query_version_reply_t *fvr;
		fvck = xcb_xfixes_query_version(xc, 2, 0);
		fvr  = xcb_xfixes_query_version_reply(xc, fvck, NULL);
		free(fvr);
	}

	ext = xcb_get_extension_data(xc, &xcb_render_id);
	if (!ext || !ext->present) {
		awm_warn("compositor: XRender extension not available");
		return -1;
	}
	comp.render_err_base     = ext->first_error;
	comp.render_request_base = ext->major_opcode;

	/* Query GLX extension opcode for error whitelisting — EGL has no X
	 * request codes, but the glx_errors stub in compositor.h still exists
	 * to avoid changing events.c; it will always return -1 after this. */

	ext = xcb_get_extension_data(xc, &xcb_shape_id);
	if (ext && ext->present) {
		comp.has_xshape     = 1;
		comp.shape_ev_base  = ext->first_event;
		comp.shape_err_base = ext->first_error;
	}

	/* --- Query X Present extension (optional) ----------------------------
	 */
	{
		const xcb_query_extension_reply_t *pext =
		    xcb_get_extension_data(xc, &xcb_present_id);
		if (pext && pext->present) {
			comp.has_present      = 1;
			comp.present_opcode   = pext->major_opcode;
			comp.present_eid_next = 1;
			awm_debug("compositor: X Present extension available "
			          "(opcode=%d)",
			    comp.present_opcode);
		}
	}

	/* Reserve event id 0 as "unset"; per-window ids start at 1. */

	/* --- Redirect all root children ---------------------------------------
	 */
	xcb_composite_redirect_subwindows(
	    xc, (xcb_window_t) root, XCB_COMPOSITE_REDIRECT_MANUAL);
	xcb_flush(xc);

	/* --- Overlay window ---------------------------------------------------
	 */
	{
		xcb_composite_get_overlay_window_cookie_t owck;
		xcb_composite_get_overlay_window_reply_t *owr;
		owck = xcb_composite_get_overlay_window(xc, (xcb_window_t) root);
		owr  = xcb_composite_get_overlay_window_reply(xc, owck, NULL);
		comp.overlay = owr ? owr->overlay_win : 0;
		free(owr);
	}
	if (!comp.overlay) {
		awm_warn("compositor: failed to get overlay window");
		xcb_composite_unredirect_subwindows(
		    xc, (xcb_window_t) root, XCB_COMPOSITE_REDIRECT_MANUAL);
		return -1;
	}

	/* Make the overlay click-through */
	{
		xcb_xfixes_region_t empty = xcb_generate_id(xc);
		xcb_xfixes_create_region(xc, empty, 0, NULL);
		xcb_xfixes_set_window_shape_region(
		    xc, (xcb_window_t) comp.overlay, XCB_SHAPE_SK_INPUT, 0, 0, empty);
		xcb_xfixes_destroy_region(xc, empty);
	}

	/* --- Initialise backend -----------------------------------------------
	 * Try EGL first; fall back to XRender.
	 */
	if (comp_backend_egl.init() == 0) {
		comp.backend = &comp_backend_egl;
		awm_debug("compositor: using EGL/GL backend");
	} else if (comp_backend_xrender.init() == 0) {
		comp.backend = &comp_backend_xrender;
		awm_debug("compositor: using XRender fallback backend");
	} else {
		awm_warn("compositor: all backends failed — compositor disabled");
		xcb_composite_release_overlay_window(xc, (xcb_window_t) root);
		xcb_composite_unredirect_subwindows(
		    xc, (xcb_window_t) root, XCB_COMPOSITE_REDIRECT_MANUAL);
		return -1;
	}

	/* --- Dirty region (starts as full screen) -----------------------------
	 */
	{
		comp.dirty = xcb_generate_id(xc);
		xcb_xfixes_create_region(xc, comp.dirty, 0, NULL);
		/* Use comp_dirty_full() so the CPU bbox is also initialised. */
		comp_dirty_full();
	}

	/* --- Subscribe overlay to Present for vsync vblank notifications -----
	 * We use a dedicated event id distinct from per-window ids.
	 * Event id 0 is reserved as "overlay vblank channel" so we allocate
	 * it first; per-window Present ids start at 1.
	 */
	if (comp.has_present) {
		comp.vblank_eid       = 0; /* overlay uses eid=0 */
		comp.present_eid_next = 1; /* per-window ids start at 1 */
		xcb_present_select_input(xc, comp.vblank_eid,
		    (xcb_window_t) comp.overlay,
		    XCB_PRESENT_EVENT_MASK_COMPLETE_NOTIFY);
		xcb_flush(xc);
		awm_debug("compositor: subscribed overlay to Present "
		          "CompleteNotify (eid=0)");
	}

	/* --- Claim _NET_WM_CM_S<n> composite manager selection ---------------
	 */
	{
		char                     sel_name[32];
		xcb_intern_atom_cookie_t ck;
		xcb_intern_atom_reply_t *r;
		snprintf(sel_name, sizeof(sel_name), "_NET_WM_CM_S%d", screen);
		ck = xcb_intern_atom(xc, 0, (uint16_t) strlen(sel_name), sel_name);
		r  = xcb_intern_atom_reply(xc, ck, NULL);
		comp.atom_cm_sn = r ? r->atom : XCB_ATOM_NONE;
		free(r);

		{
			xcb_window_t win = xcb_generate_id(xc);
			xcb_create_window(xc, XCB_COPY_FROM_PARENT, win,
			    (xcb_window_t) root, -1, -1, 1, 1, 0,
			    XCB_WINDOW_CLASS_INPUT_OUTPUT, XCB_COPY_FROM_PARENT, 0, NULL);
			comp.cm_owner_win = win;
		}

		xcb_set_selection_owner(xc, comp.cm_owner_win,
		    (xcb_atom_t) comp.atom_cm_sn, XCB_CURRENT_TIME);

		{
			xcb_get_selection_owner_cookie_t gck;
			xcb_get_selection_owner_reply_t *gr;
			gck = xcb_get_selection_owner(xc, (xcb_atom_t) comp.atom_cm_sn);
			gr  = xcb_get_selection_owner_reply(xc, gck, NULL);
			if (!gr || gr->owner != comp.cm_owner_win) {
				awm_warn("compositor: could not claim _NET_WM_CM_S%d — "
				         "another compositor may be running",
				    screen);
			} else {
				awm_debug(
				    "compositor: claimed _NET_WM_CM_S%d selection", screen);
			}
			free(gr);
		}

		{
			uint32_t evmask = XCB_EVENT_MASK_STRUCTURE_NOTIFY;
			xcb_change_window_attributes(
			    xc, comp.cm_owner_win, XCB_CW_EVENT_MASK, &evmask);
		}
	}

	/* --- Scan existing windows --------------------------------------------
	 */
	{
		xcb_query_tree_cookie_t qtck;
		xcb_query_tree_reply_t *qtr;
		uint32_t                j;

		qtck = xcb_query_tree(xc, root);
		qtr  = xcb_query_tree_reply(xc, qtck, NULL);
		if (qtr) {
			xcb_window_t *ch = xcb_query_tree_children(qtr);
			int           nc = xcb_query_tree_children_length(qtr);
			for (j = 0; j < (uint32_t) nc; j++)
				comp_add_by_xid(ch[j]);
			free(qtr);
		} else {
			awm_warn("compositor: xcb_query_tree failed on root during scan");
		}
	}

	/* --- Intern wallpaper atoms and read initial wallpaper ---------------
	 */
	{
		xcb_intern_atom_cookie_t ck0 =
		    xcb_intern_atom(xc, 0, 13, "_XROOTPMAP_ID");
		xcb_intern_atom_cookie_t ck1 =
		    xcb_intern_atom(xc, 0, 16, "ESETROOT_PMAP_ID");
		xcb_intern_atom_cookie_t ck2 =
		    xcb_intern_atom(xc, 0, 24, "_NET_WM_WINDOW_OPACITY");
		xcb_intern_atom_reply_t *r0 = xcb_intern_atom_reply(xc, ck0, NULL);
		xcb_intern_atom_reply_t *r1 = xcb_intern_atom_reply(xc, ck1, NULL);
		xcb_intern_atom_reply_t *r2 = xcb_intern_atom_reply(xc, ck2, NULL);
		comp.atom_rootpmap          = r0 ? r0->atom : XCB_ATOM_NONE;
		comp.atom_esetroot          = r1 ? r1->atom : XCB_ATOM_NONE;
		comp.atom_net_wm_opacity    = r2 ? r2->atom : XCB_ATOM_NONE;
		free(r0);
		free(r1);
		free(r2);
	}
	comp_update_wallpaper();

	comp.active = 1;

	/* Raise overlay so it sits above all windows */
	{
		uint32_t stack = XCB_STACK_MODE_ABOVE;
		xcb_configure_window(
		    xc, comp.overlay, XCB_CONFIG_WINDOW_STACK_MODE, &stack);
		xcb_map_window(xc, comp.overlay);
	}

	schedule_repaint();

	/* Suppress unused variable warning in case we add more backends later */
	(void) i;

	awm_debug("compositor: initialised (backend=%s damage_ev_base=%d)",
	    (comp.backend == &comp_backend_egl) ? "egl" : "xrender",
	    comp.damage_ev_base);
	return 0;
}

/* -------------------------------------------------------------------------
 * compositor_cleanup
 * ---------------------------------------------------------------------- */

void
compositor_cleanup(void)
{
	CompWin *cw, *next;

	if (!comp.active)
		return;

	if (comp.repaint_id) {
		g_source_remove(comp.repaint_id);
		comp.repaint_id = 0;
	}

	/* Unsubscribe the overlay from Present vblank notifications */
	if (comp.has_present && comp.overlay) {
		xcb_present_select_input(xc, comp.vblank_eid,
		    (xcb_window_t) comp.overlay, XCB_PRESENT_EVENT_MASK_NO_EVENT);
	}
	comp.vblank_armed    = 0;
	comp.repaint_pending = 0;

	/* Free all tracked windows — release backend resources first */
	for (cw = comp.windows; cw; cw = next) {
		next = cw->next;
		comp.backend->release_pixmap(cw);
		comp_free_win(cw);
		free(cw);
	}
	comp.windows = NULL;

	/* Tear down backend */
	comp.backend->release_wallpaper();
	comp.backend->cleanup();
	comp.backend = NULL;

	if (comp.overlay)
		xcb_composite_release_overlay_window(xc, (xcb_window_t) root);

	/* Release _NET_WM_CM_Sn selection */
	if (comp.cm_owner_win) {
		xcb_destroy_window(xc, comp.cm_owner_win);
		comp.cm_owner_win = 0;
	}

	if (comp.dirty)
		xcb_xfixes_destroy_region(xc, comp.dirty);

	xcb_composite_unredirect_subwindows(
	    xc, (xcb_window_t) root, XCB_COMPOSITE_REDIRECT_MANUAL);

	xcb_render_util_disconnect(xc);

	xflush();

	comp.active = 0;
}

/* -------------------------------------------------------------------------
 * Window tracking — internal helpers
 * ---------------------------------------------------------------------- */

static CompWin *
comp_find_by_xid(xcb_window_t w)
{
	CompWin *cw;
	for (cw = comp.windows; cw; cw = cw->next)
		if (cw->win == w)
			return cw;
	return NULL;
}

static CompWin *
comp_find_by_client(Client *c)
{
	CompWin *cw;
	for (cw = comp.windows; cw; cw = cw->next)
		if (cw->client == c)
			return cw;
	return NULL;
}

/* Subscribe cw to XPresent CompleteNotify events. */
static void
comp_subscribe_present(CompWin *cw)
{
	if (!comp.has_present || cw->present_eid)
		return;

	cw->present_eid = comp.present_eid_next++;

	xcb_present_select_input(xc, cw->present_eid, (xcb_window_t) cw->win,
	    XCB_PRESENT_EVENT_MASK_COMPLETE_NOTIFY);
	xcb_flush(xc);

	awm_debug("compositor: subscribed Present CompleteNotify for "
	          "window 0x%lx (eid=%u)",
	    cw->win, cw->present_eid);
}

/* Unsubscribe from Present events for cw. */
static void
comp_unsubscribe_present(CompWin *cw)
{
	if (!comp.has_present || !cw->present_eid)
		return;

	xcb_present_select_input(xc, cw->present_eid, (xcb_window_t) cw->win,
	    XCB_PRESENT_EVENT_MASK_NO_EVENT);
	xcb_flush(xc);
	cw->present_eid = 0;
}

static void
comp_free_win(CompWin *cw)
{
	if (comp.has_xshape) {
		xcb_void_cookie_t    ck;
		xcb_generic_error_t *err;

		ck  = xcb_shape_select_input_checked(xc, (xcb_window_t) cw->win, 0);
		err = xcb_request_check(xc, ck);
		free(err);
	}

	comp_unsubscribe_present(cw);

	if (cw->damage) {
		xcb_void_cookie_t    ck;
		xcb_generic_error_t *err;

		ck  = xcb_damage_destroy_checked(xc, cw->damage);
		err = xcb_request_check(xc, ck);
		free(err);
		cw->damage = 0;
	}
	/* picture is owned by the XRender backend; it calls
	 * xcb_render_free_picture in release_pixmap().  For the EGL backend
	 * cw->picture is always 0. We clear it here as a safety measure in case
	 * comp_free_win is called without a prior release_pixmap (e.g. on a window
	 * that was never fully initialised). */
	if (cw->picture) {
		xcb_render_free_picture(xc, cw->picture);
		cw->picture = 0;
	}
	if (cw->pixmap) {
		xcb_free_pixmap(xc, cw->pixmap);
		cw->pixmap = 0;
	}
}

static void
comp_refresh_pixmap(CompWin *cw)
{
	/* Release backend resources before freeing the pixmap */
	comp.backend->release_pixmap(cw);

	if (cw->pixmap) {
		xcb_free_pixmap(xc, cw->pixmap);
		cw->pixmap = 0;
	}

	/* New pixmap — require a full dirty on its first damage notification. */
	cw->ever_damaged = 0;

	{
		xcb_pixmap_t         pix = xcb_generate_id(xc);
		xcb_void_cookie_t    ck;
		xcb_generic_error_t *err;

		ck = xcb_composite_name_window_pixmap_checked(
		    xc, (xcb_window_t) cw->win, pix);
		xcb_flush(xc);
		err = xcb_request_check(xc, ck);
		free(err);
		cw->pixmap = pix;
	}

	if (!cw->pixmap)
		return;

	{
		xcb_get_geometry_cookie_t gck;
		xcb_get_geometry_reply_t *gr;
		gck = xcb_get_geometry(xc, (xcb_drawable_t) cw->pixmap);
		gr  = xcb_get_geometry_reply(xc, gck, NULL);
		if (!gr) {
			awm_warn("compositor: pixmap geometry query failed — "
			         "releasing stale pixmap");
			xcb_free_pixmap(xc, cw->pixmap);
			cw->pixmap = 0;
			return;
		}
		free(gr);
	}

	comp.backend->bind_pixmap(cw);
}

/* Read _XROOTPMAP_ID (or ESETROOT_PMAP_ID fallback) and rebuild wallpaper. */
static void
comp_update_wallpaper(void)
{
	xcb_pixmap_t              pmap = 0;
	xcb_atom_t                atoms[2];
	int                       i;
	xcb_get_property_cookie_t ck;
	xcb_get_property_reply_t *r;

	/* Release previous wallpaper resources in the backend */
	comp.backend->release_wallpaper();
	comp.wallpaper_pixmap = 0;

	atoms[0] = comp.atom_rootpmap;
	atoms[1] = comp.atom_esetroot;

	for (i = 0; i < 2 && pmap == 0; i++) {
		ck = xcb_get_property(xc, 0, (xcb_window_t) root,
		    (xcb_atom_t) atoms[i], XCB_ATOM_PIXMAP, 0, 1);
		r  = xcb_get_property_reply(xc, ck, NULL);
		if (r &&
		    xcb_get_property_value_length(r) >= (int) sizeof(xcb_pixmap_t))
			pmap = (xcb_pixmap_t) * (xcb_pixmap_t *) xcb_get_property_value(r);
		free(r);
	}

	if (pmap == 0)
		return;

	comp.wallpaper_pixmap = pmap;
	comp.backend->update_wallpaper();
}

static void
comp_restack_above(CompWin *cw, xcb_window_t above_xid)
{
	CompWin *prev = NULL, *cur;
	CompWin *above_cw;
	CompWin *ins_prev;

	for (cur = comp.windows; cur; cur = cur->next) {
		if (cur == cw) {
			if (prev)
				prev->next = cw->next;
			else
				comp.windows = cw->next;
			cw->next = NULL;
			break;
		}
		prev = cur;
	}

	if (above_xid == 0) {
		cw->next     = comp.windows;
		comp.windows = cw;
		return;
	}

	above_cw = comp_find_by_xid(above_xid);
	if (!above_cw) {
		ins_prev = NULL;
		for (cur = comp.windows; cur; cur = cur->next)
			ins_prev = cur;
		if (ins_prev) {
			cw->next       = ins_prev->next;
			ins_prev->next = cw;
		} else {
			cw->next     = comp.windows;
			comp.windows = cw;
		}
		return;
	}

	cw->next       = above_cw->next;
	above_cw->next = cw;
}

static void
comp_add_by_xid(xcb_window_t w)
{
	CompWin *cw;

	if (comp_find_by_xid(w))
		return;

	if (w == comp.overlay)
		return;

	{
		xcb_get_window_attributes_cookie_t wac =
		    xcb_get_window_attributes(xc, (xcb_window_t) w);
		xcb_get_geometry_cookie_t gc =
		    xcb_get_geometry(xc, (xcb_drawable_t) w);
		xcb_get_window_attributes_reply_t *war =
		    xcb_get_window_attributes_reply(xc, wac, NULL);
		xcb_get_geometry_reply_t *gr = xcb_get_geometry_reply(xc, gc, NULL);

		if (!war || !gr) {
			free(war);
			free(gr);
			return;
		}
		if (war->_class == XCB_WINDOW_CLASS_INPUT_ONLY) {
			free(war);
			free(gr);
			return;
		}
		if (war->map_state != XCB_MAP_STATE_VIEWABLE) {
			free(war);
			free(gr);
			return;
		}

		cw = (CompWin *) calloc(1, sizeof(CompWin));
		if (!cw) {
			free(war);
			free(gr);
			return;
		}

		cw->win   = w;
		cw->x     = gr->x;
		cw->y     = gr->y;
		cw->w     = gr->width;
		cw->h     = gr->height;
		cw->bw    = gr->border_width;
		cw->depth = gr->depth;
		cw->argb  = (gr->depth == 32);
		free(war);
		free(gr);
	}
	cw->opacity    = 1.0;
	cw->redirected = 1;
	cw->egl_image  = EGL_NO_IMAGE_KHR;

	{
		Client  *c;
		Monitor *m;
		cw->client = NULL;
		for (m = mons; m; m = m->next)
			for (c = cl->clients; c; c = c->next)
				if (c->win == w) {
					cw->client = c;
					break;
				}
		if (cw->client)
			cw->opacity = cw->client->opacity;
	}

	comp_refresh_pixmap(cw);

	if (cw->pixmap) {
		xcb_void_cookie_t    ck;
		xcb_generic_error_t *err;

		cw->damage = xcb_generate_id(xc);
		ck = xcb_damage_create_checked(xc, cw->damage, (xcb_drawable_t) w,
		    XCB_DAMAGE_REPORT_LEVEL_NON_EMPTY);
		xcb_flush(xc);
		err = xcb_request_check(xc, ck);
		free(err);
	}

	comp_subscribe_present(cw);

	if (comp.has_xshape)
		xcb_shape_select_input(xc, (xcb_window_t) w, 1);

	if (!comp.windows) {
		cw->next     = NULL;
		comp.windows = cw;
	} else {
		CompWin *tail = comp.windows;
		while (tail->next)
			tail = tail->next;
		tail->next = cw;
		cw->next   = NULL;
	}
}

/* -------------------------------------------------------------------------
 * Public API — called from WM core
 * ---------------------------------------------------------------------- */

void
compositor_add_window(Client *c)
{
	CompWin *cw;

	if (!comp.active || !c)
		return;

	cw = comp_find_by_xid(c->win);
	if (cw) {
		cw->client  = c;
		cw->opacity = c->opacity;
		return;
	}

	comp_add_by_xid(c->win);

	cw = comp_find_by_xid(c->win);
	if (cw) {
		cw->client  = c;
		cw->opacity = c->opacity;
	}

	schedule_repaint();
}

void
compositor_remove_window(Client *c)
{
	CompWin *cw, *prev;

	if (!comp.active || !c)
		return;

	prev = NULL;
	for (cw = comp.windows; cw; cw = cw->next) {
		if (cw->client == c || cw->win == c->win) {
			comp_dirty_add_rect(
			    cw->x, cw->y, cw->w + 2 * cw->bw, cw->h + 2 * cw->bw);
			if (prev)
				prev->next = cw->next;
			else
				comp.windows = cw->next;
			comp.backend->release_pixmap(cw);
			comp_free_win(cw);
			free(cw);
			schedule_repaint();
			return;
		}
		prev = cw;
	}
}

void
compositor_configure_window(Client *c, int actual_bw)
{
	CompWin *cw;
	int      resized;

	if (!comp.active || !c)
		return;

	cw = comp_find_by_client(c);
	if (!cw)
		return;

	/* Dirty old position */
	comp_dirty_add_rect(cw->x, cw->y, cw->w + 2 * cw->bw, cw->h + 2 * cw->bw);

	resized = (c->w != cw->w || c->h != cw->h);

	cw->x  = c->x - actual_bw;
	cw->y  = c->y - actual_bw;
	cw->w  = c->w;
	cw->h  = c->h;
	cw->bw = actual_bw;

	/* Dirty new position */
	comp_dirty_add_rect(cw->x, cw->y, cw->w + 2 * cw->bw, cw->h + 2 * cw->bw);

	if (cw->redirected && resized)
		comp_refresh_pixmap(cw);

	schedule_repaint();
}

void
compositor_bypass_window(Client *c, int bypass)
{
	CompWin *cw;

	if (!comp.active || !c)
		return;

	cw = comp_find_by_client(c);
	if (!cw)
		return;

	if (bypass == !cw->redirected)
		return;

	if (bypass) {
		xcb_void_cookie_t    ck;
		xcb_generic_error_t *err;

		ck = xcb_composite_unredirect_window_checked(
		    xc, (xcb_window_t) c->win, XCB_COMPOSITE_REDIRECT_MANUAL);
		err = xcb_request_check(xc, ck);
		free(err);
		cw->redirected = 0;
		comp.backend->release_pixmap(cw);
		comp_free_win(cw);
	} else {
		xcb_void_cookie_t    ck;
		xcb_generic_error_t *err;

		ck = xcb_composite_redirect_window_checked(
		    xc, (xcb_window_t) c->win, XCB_COMPOSITE_REDIRECT_MANUAL);
		err = xcb_request_check(xc, ck);
		free(err);
		cw->redirected = 1;
		comp_refresh_pixmap(cw);
		if (cw->pixmap && !cw->damage) {
			cw->damage = xcb_generate_id(xc);
			xcb_damage_create(xc, cw->damage, (xcb_drawable_t) c->win,
			    XCB_DAMAGE_REPORT_LEVEL_NON_EMPTY);
		}
		comp_subscribe_present(cw);
	}

	schedule_repaint();
}

/*
 * State for the deferred fullscreen bypass callback.
 */
static xcb_window_t comp_pending_bypass_win = XCB_NONE;
static guint        comp_pending_bypass_id  = 0;

static gboolean
comp_fullscreen_bypass_cb(gpointer data)
{
	xcb_window_t win = (xcb_window_t) (uintptr_t) data;
	Client      *c   = NULL;
	Monitor     *m;

	comp_pending_bypass_id  = 0;
	comp_pending_bypass_win = XCB_NONE;

	if (!comp.active)
		return G_SOURCE_REMOVE;

	for (m = mons; m; m = m->next) {
		Client *tc;
		for (tc = m->cl->clients; tc; tc = tc->next) {
			if (tc->win == win) {
				c = tc;
				break;
			}
		}
		if (c)
			break;
	}

	if (!c || !c->isfullscreen)
		return G_SOURCE_REMOVE;

	compositor_bypass_window(c, 1);

	{
		uint32_t vals[2] = { (uint32_t) c->mon->barwin, XCB_STACK_MODE_ABOVE };
		xcb_configure_window(xc, c->win,
		    XCB_CONFIG_WINDOW_SIBLING | XCB_CONFIG_WINDOW_STACK_MODE, vals);
	}

	compositor_check_unredirect();
	xcb_clear_area(xc, 1, (xcb_window_t) c->win, 0, 0, 0, 0);
	xcb_flush(xc);

	return G_SOURCE_REMOVE;
}

void
compositor_defer_fullscreen_bypass(Client *c)
{
	if (!comp.active || !c)
		return;

	if (comp_pending_bypass_id) {
		g_source_remove(comp_pending_bypass_id);
		comp_pending_bypass_id  = 0;
		comp_pending_bypass_win = XCB_NONE;
	}

	comp_pending_bypass_win = c->win;
	comp_pending_bypass_id  = g_timeout_add(
        40, comp_fullscreen_bypass_cb, (gpointer) (uintptr_t) c->win);
}

void
compositor_set_opacity(Client *c, unsigned long raw)
{
	CompWin *cw;

	if (!comp.active || !c)
		return;

	cw = comp_find_by_client(c);
	if (!cw)
		return;

	cw->opacity = (raw == 0) ? 0.0 : (double) raw / (double) 0xFFFFFFFFUL;
	c->opacity  = cw->opacity;
	schedule_repaint();
}

void
compositor_focus_window(Client *c)
{
	CompWin *cw;

	if (!comp.active || !c)
		return;

	cw = comp_find_by_client(c);
	if (!cw || cw->bw <= 0)
		return;

	comp_dirty_add_rect(cw->x, cw->y, cw->w + 2 * cw->bw, cw->h + 2 * cw->bw);
	schedule_repaint();
}

void
compositor_set_hidden(Client *c, int hidden)
{
	CompWin *cw;

	if (!comp.active || !c)
		return;

	cw = comp_find_by_client(c);
	if (!cw)
		return;

	if (cw->hidden == hidden)
		return;

	cw->hidden = hidden;
	comp_dirty_add_rect(cw->x, cw->y, cw->w + 2 * cw->bw, cw->h + 2 * cw->bw);
	schedule_repaint();
}

void
compositor_damage_all(void)
{
	if (!comp.active)
		return;

	comp_dirty_full();
	schedule_repaint();
}

void
compositor_notify_screen_resize(void)
{
	if (!comp.active)
		return;

	comp.backend->notify_resize();
	compositor_damage_all();
}

void
compositor_raise_overlay(void)
{
	if (!comp.active)
		return;
	{
		uint32_t stack = XCB_STACK_MODE_ABOVE;
		xcb_configure_window(
		    xc, comp.overlay, XCB_CONFIG_WINDOW_STACK_MODE, &stack);
	}
}

void
compositor_check_unredirect(void)
{
	Client *sel;
	int     should_pause;

	if (!comp.active)
		return;

	sel          = selmon ? selmon->sel : NULL;
	should_pause = 0;

	if (sel && sel->isfullscreen && sel->opacity >= 1.0 &&
	    sel->x == sel->mon->mx && sel->y == sel->mon->my &&
	    sel->w == sel->mon->mw && sel->h == sel->mon->mh) {
		should_pause = 1;
	}

	if (should_pause == comp.paused)
		return;

	comp.paused = should_pause;

	if (comp.paused) {
		if (comp.repaint_id) {
			g_source_remove(comp.repaint_id);
			comp.repaint_id = 0;
		}
		comp.vblank_armed    = 0;
		comp.repaint_pending = 0;
		{
			CompWin *cw;
			for (cw = comp.windows; cw; cw = cw->next) {
				if (cw->client && cw->client->isfullscreen && cw->redirected) {
					xcb_void_cookie_t    ck;
					xcb_generic_error_t *err;

					ck  = xcb_composite_unredirect_window_checked(xc,
					     (xcb_window_t) cw->win, XCB_COMPOSITE_REDIRECT_MANUAL);
					err = xcb_request_check(xc, ck);
					free(err);
					cw->redirected = 0;
					comp.backend->release_pixmap(cw);
					comp_free_win(cw);
				}
			}
		}
		{
			uint32_t stack = XCB_STACK_MODE_BELOW;
			xcb_configure_window(
			    xc, comp.overlay, XCB_CONFIG_WINDOW_STACK_MODE, &stack);
		}
		awm_debug("compositor: suspended (fullscreen unredirect)");
	} else {
		CompWin *cw;
		for (cw = comp.windows; cw; cw = cw->next) {
			if (cw->client && !cw->redirected) {
				xcb_void_cookie_t    ck;
				xcb_generic_error_t *err;

				if (cw->client->bypass_compositor == 1)
					continue;

				ck = xcb_composite_redirect_window_checked(
				    xc, (xcb_window_t) cw->win, XCB_COMPOSITE_REDIRECT_MANUAL);
				err = xcb_request_check(xc, ck);
				free(err);
				cw->redirected = 1;
				comp_refresh_pixmap(cw);
				if (cw->pixmap && !cw->damage) {
					cw->damage = xcb_generate_id(xc);
					xcb_damage_create(xc, cw->damage, (xcb_drawable_t) cw->win,
					    XCB_DAMAGE_REPORT_LEVEL_NON_EMPTY);
				}
				comp_subscribe_present(cw);
				awm_debug("compositor: re-redirected fullscreen "
				          "window 0x%lx on resume",
				    cw->win);
			}
		}
		{
			uint32_t stack = XCB_STACK_MODE_ABOVE;
			xcb_configure_window(
			    xc, comp.overlay, XCB_CONFIG_WINDOW_STACK_MODE, &stack);
		}
		compositor_damage_all();
		awm_debug("compositor: resumed");
	}
}

void
compositor_xrender_errors(int *req_base, int *err_base)
{
	if (!comp.active) {
		*req_base = -1;
		*err_base = -1;
		return;
	}
	*req_base = comp.render_request_base;
	*err_base = comp.render_err_base;
}

void
compositor_damage_errors(int *req_base, int *err_base)
{
	*req_base = comp.damage_req_base;
	*err_base = comp.damage_err_base;
}

void
compositor_glx_errors(int *req_base, int *err_base)
{
	*req_base = -1;
	*err_base = -1;
}

void
compositor_present_errors(int *req_base)
{
	*req_base = (int) (uint8_t) comp.present_opcode;
}

void
compositor_repaint_now(void)
{
	if (!comp.active)
		return;
	if (comp.repaint_id) {
		g_source_remove(comp.repaint_id);
		comp.repaint_id = 0;
	}
	comp_do_repaint();
}

/* -------------------------------------------------------------------------
 * Event handler
 * ---------------------------------------------------------------------- */

void
compositor_handle_event(xcb_generic_event_t *ev)
{
	if (!comp.active)
		return;

	if ((ev->response_type & ~0x80) ==
	    (uint8_t) (comp.damage_ev_base + XCB_DAMAGE_NOTIFY)) {
		xcb_damage_notify_event_t *dev =
		    (xcb_damage_notify_event_t *) (void *) ev;
		CompWin *dcw = comp_find_by_xid(dev->drawable);

		if (!dcw) {
			xcb_void_cookie_t    ck;
			xcb_generic_error_t *err;

			ck = xcb_damage_subtract_checked(
			    xc, dev->damage, XCB_NONE, XCB_NONE);
			err = xcb_request_check(xc, ck);
			free(err);
			schedule_repaint();
			return;
		}

		if (!dcw->ever_damaged) {
			xcb_void_cookie_t    ck;
			xcb_generic_error_t *err;

			dcw->ever_damaged = 1;
			ck                = xcb_damage_subtract_checked(
                xc, dev->damage, XCB_NONE, XCB_NONE);
			err = xcb_request_check(xc, ck);
			free(err);
			comp_dirty_add_rect(
			    dcw->x, dcw->y, dcw->w + 2 * dcw->bw, dcw->h + 2 * dcw->bw);
		} else {
			xcb_xfixes_region_t  dmg_region = xcb_generate_id(xc);
			xcb_void_cookie_t    ck;
			xcb_generic_error_t *err;

			xcb_xfixes_create_region(xc, dmg_region, 0, NULL);
			ck = xcb_damage_subtract_checked(
			    xc, dev->damage, XCB_NONE, dmg_region);
			err = xcb_request_check(xc, ck);
			free(err);
			/* Translate damage region to screen coords and union into
			 * comp.dirty (server-side).  The CPU bbox will be conservatively
			 * expanded to the whole window below — precise per-damage bbox
			 * tracking would require another round-trip. */
			xcb_xfixes_translate_region(
			    xc, dmg_region, (int16_t) dcw->x, (int16_t) dcw->y);
			xcb_xfixes_union_region(xc, comp.dirty, dmg_region, comp.dirty);
			xcb_xfixes_destroy_region(xc, dmg_region);
			/* Extend CPU bbox by the window rect (conservative) */
			comp_dirty_add_rect(
			    dcw->x, dcw->y, dcw->w + 2 * dcw->bw, dcw->h + 2 * dcw->bw);
		}

		schedule_repaint();
		return;
	}

	{
		uint8_t type = ev->response_type & ~0x80;

		if (type == XCB_MAP_NOTIFY) {
			xcb_map_notify_event_t *mev = (xcb_map_notify_event_t *) ev;
			if (mev->event == root)
				comp_add_by_xid(mev->window);
			schedule_repaint();
			return;
		}

		if (type == XCB_UNMAP_NOTIFY) {
			xcb_unmap_notify_event_t *uev = (xcb_unmap_notify_event_t *) ev;
			CompWin                  *cw  = comp_find_by_xid(uev->window);
			if (cw && !cw->client) {
				CompWin *prev = NULL, *cur;
				comp_dirty_add_rect(
				    cw->x, cw->y, cw->w + 2 * cw->bw, cw->h + 2 * cw->bw);
				for (cur = comp.windows; cur; cur = cur->next) {
					if (cur == cw) {
						if (prev)
							prev->next = cw->next;
						else
							comp.windows = cw->next;
						comp.backend->release_pixmap(cw);
						comp_free_win(cw);
						free(cw);
						break;
					}
					prev = cur;
				}
			}
			schedule_repaint();
			return;
		}

		if (type == XCB_CONFIGURE_NOTIFY) {
			xcb_configure_notify_event_t *cev =
			    (xcb_configure_notify_event_t *) ev;
			CompWin *cw = comp_find_by_xid(cev->window);
			if (cw) {
				if (cw->client) {
					comp_restack_above(cw, cev->above_sibling);
					schedule_repaint();
					return;
				}

				int resized = (cev->width != cw->w || cev->height != cw->h);

				comp_dirty_add_rect(
				    cw->x, cw->y, cw->w + 2 * cw->bw, cw->h + 2 * cw->bw);

				cw->x  = cev->x;
				cw->y  = cev->y;
				cw->w  = cev->width;
				cw->h  = cev->height;
				cw->bw = cev->border_width;

				comp_restack_above(cw, cev->above_sibling);

				if (cw->redirected && resized)
					comp_refresh_pixmap(cw);

				schedule_repaint();
			}
			return;
		}

		if (type == XCB_DESTROY_NOTIFY) {
			xcb_destroy_notify_event_t *dev =
			    (xcb_destroy_notify_event_t *) ev;
			CompWin *prev = NULL, *cw;
			for (cw = comp.windows; cw; cw = cw->next) {
				if (cw->win == dev->window) {
					comp_dirty_add_rect(
					    cw->x, cw->y, cw->w + 2 * cw->bw, cw->h + 2 * cw->bw);
					if (prev)
						prev->next = cw->next;
					else
						comp.windows = cw->next;
					comp.backend->release_pixmap(cw);
					comp_free_win(cw);
					free(cw);
					schedule_repaint();
					return;
				}
				prev = cw;
			}
			return;
		}

		if (type == XCB_PROPERTY_NOTIFY) {
			xcb_property_notify_event_t *pev =
			    (xcb_property_notify_event_t *) ev;
			if (pev->window == root &&
			    (pev->atom == comp.atom_rootpmap ||
			        pev->atom == comp.atom_esetroot)) {
				comp_update_wallpaper();
				compositor_damage_all();
			} else if (pev->atom == comp.atom_net_wm_opacity &&
			    pev->window != root) {
				CompWin *cw = comp_find_by_xid(pev->window);
				if (cw && cw->client) {
					xcb_get_property_cookie_t ck2;
					xcb_get_property_reply_t *r2;

					ck2 = xcb_get_property(xc, 0, (xcb_window_t) pev->window,
					    (xcb_atom_t) comp.atom_net_wm_opacity,
					    XCB_ATOM_CARDINAL, 0, 1);
					r2  = xcb_get_property_reply(xc, ck2, NULL);
					if (r2 &&
					    xcb_get_property_value_length(r2) >=
					        (int) sizeof(uint32_t)) {
						unsigned long raw =
						    *(uint32_t *) xcb_get_property_value(r2);
						compositor_set_opacity(cw->client, raw);
					} else {
						compositor_set_opacity(cw->client, 0xFFFFFFFFUL);
					}
					free(r2);
				}
			}
			return;
		}

		if (comp.has_xshape &&
		    type == (uint8_t) (comp.shape_ev_base + XCB_SHAPE_NOTIFY)) {
			xcb_shape_notify_event_t *sev =
			    (xcb_shape_notify_event_t *) (void *) ev;
			if (sev->shape_kind == XCB_SHAPE_SK_BOUNDING) {
				CompWin *cw = comp_find_by_xid(sev->affected_window);
				if (cw) {
					if (comp.backend == &comp_backend_egl) {
						/* GL path: re-acquire pixmap so TFP reflects
						 * the new shape. */
						if (cw->redirected)
							comp_refresh_pixmap(cw);
					} else if (cw->picture) {
						comp_xrender_apply_shape(cw);
					}
					schedule_repaint();
				}
			}
			return;
		}

		if (type == XCB_SELECTION_CLEAR) {
			xcb_selection_clear_event_t *sce =
			    (xcb_selection_clear_event_t *) ev;
			if (sce->selection == comp.atom_cm_sn) {
				awm_warn(
				    "compositor: lost _NET_WM_CM_S%d selection to another "
				    "compositor; disabling compositing",
				    screen);
				compositor_cleanup();
			}
			return;
		}

		if (comp.has_present && type == XCB_GE_GENERIC) {
			xcb_ge_generic_event_t *ge = (xcb_ge_generic_event_t *) ev;
			if (ge->extension == (uint8_t) comp.present_opcode &&
			    ge->event_type == XCB_PRESENT_COMPLETE_NOTIFY) {
				xcb_present_complete_notify_event_t *pev =
				    (xcb_present_complete_notify_event_t *) ev;

				/* --- Overlay vblank notification (eid=0) ----------------
				 * The vblank just fired.  If there is pending damage, paint
				 * now and re-arm.  Otherwise stop the vblank loop — it will
				 * restart when schedule_repaint() is next called.
				 */
				if (pev->event == comp.vblank_eid) {
					comp.vblank_armed = 0;
					if (!comp.paused && comp.repaint_pending) {
						comp.repaint_pending = 0;
						comp_do_repaint();
						/* Re-arm immediately for the next vblank so any
						 * damage that arrived during rendering is caught. */
						comp_arm_vblank();
					}
					return;
				}

				/* --- Per-window Present CompleteNotify (TFP refresh) ---- */
				if (pev->kind == XCB_PRESENT_COMPLETE_KIND_PIXMAP) {
					CompWin *cw = comp_find_by_xid(pev->window);
					if (cw && cw->redirected && !comp.paused) {
						comp_refresh_pixmap(cw);
						compositor_damage_all();
						schedule_repaint();
						awm_debug("compositor: Present CompleteNotify on "
						          "window 0x%lx — refreshed pixmap",
						    cw->win);
					}
				}
			}
			return;
		}

	} /* end type switch block */
}

/* -------------------------------------------------------------------------
 * Repaint scheduler and vblank loop
 * ---------------------------------------------------------------------- */

/*
 * Arm one vblank notification via xcb_present_notify_msc.
 *
 * We request the very next MSC (target_msc=0, divisor=0, remainder=0 means
 * "fire at the next vblank").  When the server sends PresentCompleteNotify
 * with event_id == comp.vblank_eid, we paint and re-arm if needed.
 *
 * Falls back to the legacy g_idle_add path if X Present is unavailable.
 */
static void
comp_arm_vblank(void)
{
	if (!comp.active || comp.paused)
		return;

	if (comp.has_present && !comp.vblank_armed) {
		xcb_present_notify_msc(xc, (xcb_window_t) comp.overlay,
		    comp.vblank_eid,
		    /* target_msc */ 0,
		    /* divisor    */ 0,
		    /* remainder  */ 0);
		xcb_flush(xc);
		comp.vblank_armed = 1;
		return;
	}

	/* No Present: fall back to an immediate idle repaint */
	if (!comp.repaint_id) {
		comp.repaint_id = g_idle_add_full(
		    G_PRIORITY_HIGH_IDLE, comp_repaint_idle, NULL, NULL);
	}
}

static void
schedule_repaint(void)
{
	if (!comp.active || comp.paused)
		return;

	comp.repaint_pending = 1;
	comp_arm_vblank();
}

/* -------------------------------------------------------------------------
 * Repaint — fallback idle callback (used only when Present is unavailable)
 * ---------------------------------------------------------------------- */

static gboolean
comp_repaint_idle(gpointer data)
{
	(void) data;
	comp.repaint_id      = 0;
	comp.repaint_pending = 0;

	if (!comp.active || comp.paused)
		return G_SOURCE_REMOVE;

	comp_do_repaint();
	return G_SOURCE_REMOVE;
}

static void
comp_do_repaint(void)
{
	if (!comp.active || comp.paused)
		return;

	comp.backend->repaint();
}

#endif /* COMPOSITOR */

/* Satisfy ISO C99: a translation unit must contain at least one declaration */
typedef int compositor_translation_unit_nonempty;
