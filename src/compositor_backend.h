/* compositor_backend.h — internal header shared between compositor.c,
 * compositor_egl.c and compositor_xrender.c.
 *
 * NOT part of the public API.  Do not include from files outside the
 * compositor implementation.
 *
 * Only compiled when -DCOMPOSITOR is active.
 */

#ifndef COMPOSITOR_BACKEND_H
#define COMPOSITOR_BACKEND_H

#ifdef COMPOSITOR

#include <stdint.h>

#include <xcb/xcb.h>
#include <xcb/damage.h>
#include <xcb/render.h>
#include <xcb/xfixes.h>
#include <xcb/present.h>
#include <xcb/xcb_renderutil.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>

#include <glib.h>

#include "awm.h" /* Client, Monitor, selmon, xc, root, screen, sw, sh, ... */

/* -------------------------------------------------------------------------
 * CompWin — per-window compositor state
 * ---------------------------------------------------------------------- */

typedef struct CompWin {
	xcb_window_t win;
	Client      *client; /* NULL for override_redirect windows         */
	xcb_pixmap_t pixmap; /* XCompositeNameWindowPixmap result           */
	/* XRender path (fallback) */
	xcb_render_picture_t picture; /* XRenderCreatePicture on pixmap  */
	/* GL/EGL path */
	EGLImageKHR egl_image; /* EGL image wrapping pixmap (KHR_image_pixmap) */
	GLuint      texture;   /* GL_TEXTURE_2D bound via EGL image             */
	xcb_damage_damage_t damage;
	int    x, y, w, h, bw; /* last known geometry                 */
	int    depth;          /* window depth                              */
	int    argb;           /* depth == 32                               */
	double opacity;        /* 0.0 – 1.0                                 */
	int    redirected;     /* 0 = bypass (fullscreen/bypass-hint)    */
	int    hidden;         /* 1 = moved off-screen by showhide()        */
	int    ever_damaged;   /* 0 = no damage received yet (since map)  */
	xcb_present_event_t present_eid; /* 0 = not subscribed to Present events */
	struct CompWin     *next;
} CompWin;

/* -------------------------------------------------------------------------
 * CompBackend — vtable for EGL and XRender backends
 * ---------------------------------------------------------------------- */

typedef struct CompBackend {
	/* Initialise the backend.  Called from compositor_init() after the overlay
	 * window is created and all shared state is populated.
	 * Returns 0 on success, -1 to signal that this backend is unavailable
	 * (compositor_init() will try the next backend). */
	int (*init)(void);

	/* Tear down all backend-private resources.  Called from
	 * compositor_cleanup() before the shared cleanup block. */
	void (*cleanup)(void);

	/* Bind a window pixmap for this backend.  Called from
	 * comp_refresh_pixmap() after cw->pixmap is freshly acquired.
	 * The EGL backend builds an EGLImageKHR + GL texture;
	 * the XRender backend creates an XRender Picture. */
	void (*bind_pixmap)(CompWin *cw);

	/* Release a window pixmap binding.  Called from comp_free_win() and
	 * before a pixmap refresh.  Must be safe to call with no binding held
	 * (cw->texture==0 / cw->picture==0). */
	void (*release_pixmap)(CompWin *cw);

	/* Build (or rebuild) the wallpaper resource from comp.wallpaper_pixmap.
	 * Called from comp_update_wallpaper() after the pixmap XID has been read.
	 * comp.wallpaper_pixmap is always set before this is called. */
	void (*update_wallpaper)(void);

	/* Release the wallpaper resource.  Called at the start of
	 * comp_update_wallpaper() before rebuilding, and from
	 * compositor_cleanup(). */
	void (*release_wallpaper)(void);

	/* Execute one full repaint.  Called from comp_do_repaint(). */
	void (*repaint)(void);

	/* Handle a screen resize (sw/sh already updated).
	 * Called from compositor_notify_screen_resize(). */
	void (*notify_resize)(void);
} CompBackend;

/* Backend singletons — defined in compositor_egl.c / compositor_xrender.c */
extern const CompBackend comp_backend_egl;
extern const CompBackend comp_backend_xrender;

/* -------------------------------------------------------------------------
 * Shared compositor state — defined (static) in compositor.c, accessed by
 * the backends via the accessor functions below or via the extern below.
 *
 * Backends must NOT declare their own copy of this struct.  They access
 * shared state through comp_shared (extern pointer to the compositor.c static
 * struct) so the linker can resolve the single definition.
 *
 * Fields used ONLY by one backend are kept in that backend's own static
 * state struct (CompEGLState / CompXRenderState).
 * ---------------------------------------------------------------------- */

typedef struct CompShared {
	int          active;
	xcb_window_t overlay;

	/* Damage tracking */
	int   damage_ev_base;
	int   damage_err_base;
	int   damage_req_base;
	int   xfixes_ev_base;
	int   xfixes_err_base;
	guint repaint_id;          /* GLib idle source id, 0 = none            */
	int   paused;              /* 1 = overlay hidden, repaints suppressed  */
	xcb_xfixes_region_t dirty; /* accumulated dirty region (server-side)   */

	/* CPU-side dirty bounding box — updated whenever dirty is modified.
	 * Avoids a synchronous xcb_xfixes_fetch_region round-trip per frame.
	 * dirty_bbox_valid=0 means a full-screen repaint is required. */
	int dirty_bbox_valid;
	int dirty_x1, dirty_y1, dirty_x2, dirty_y2; /* screen coords, inclusive */

	/* Present-based vsync — overlay vblank loop.
	 * vblank_eid : event id used to subscribe the overlay to Present.
	 * vblank_armed : 1 = a notify_msc request is in-flight; a
	 *                    PresentCompleteNotify will arrive at next vblank.
	 * repaint_pending : 1 = damage has accumulated; paint on next vblank. */
	xcb_present_event_t vblank_eid;
	int                 vblank_armed;
	int                 repaint_pending;

	CompWin      *windows;
	GMainContext *ctx;

	/* Wallpaper */
	xcb_atom_t   atom_rootpmap;
	xcb_atom_t   atom_esetroot;
	xcb_pixmap_t wallpaper_pixmap; /* raw X pixmap XID (both paths)        */

	/* XRender extension codes — needed for error whitelisting */
	int render_request_base;
	int render_err_base;

	/* XShape extension — optional */
	int has_xshape;
	int shape_ev_base;
	int shape_err_base;

	/* X Present extension — optional */
	int                 has_present;
	uint8_t             present_opcode;
	xcb_present_event_t present_eid_next;

	/* _NET_WM_CM_Sn selection ownership */
	xcb_window_t cm_owner_win;
	xcb_atom_t   atom_cm_sn;

	/* Per-window opacity atom */
	xcb_atom_t atom_net_wm_opacity;

	/* XRender picture format cache */
	const xcb_render_query_pict_formats_reply_t *render_formats;

	/* Active backend (set during compositor_init) */
	const CompBackend *backend;
} CompShared;

/* The single instance — defined in compositor.c */
extern CompShared comp;

/* -------------------------------------------------------------------------
 * Inline dirty-bbox helpers — used by compositor.c and both backends.
 * ---------------------------------------------------------------------- */

static inline void
comp_dirty_clear(void)
{
	xcb_xfixes_set_region(xc, comp.dirty, 0, NULL);
	comp.dirty_bbox_valid = 0;
	comp.dirty_x1 = comp.dirty_y1 = 0;
	comp.dirty_x2 = comp.dirty_y2 = 0;
}

#endif /* COMPOSITOR */
#endif /* COMPOSITOR_BACKEND_H */
