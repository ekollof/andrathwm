/* switcher.c — Alt+Tab / Super+Tab window switcher for awm
 *
 * Architecture:
 *   - A persistent (initially hidden) GTK floating window is created once in
 *     switcher_init() and reused on every invocation.
 *   - switcher_show() collects the current client list, builds XRender
 *     pre-scaled thumbnail pixmaps for each client, populates a row of
 *     GtkDrawingArea cards inside a GtkScrolledWindow, and makes the window
 *     visible.
 *   - Cycling (Tab / Shift+Tab) is driven from awm's keypress() handler via
 *     switcher_next() / switcher_prev(), because awm holds the X passive grab
 *     on those keybindings and GTK never sees the key events.
 *   - Confirmation (release of Alt/Super) is driven from awm's keyrelease()
 *     handler via switcher_confirm_xkb().
 *   - Escape calls switcher_cancel_xkb() also from keypress().
 *
 * Scope:
 *   - Alt+Tab  (all_monitors=0): all windows currently visible on any
 *     monitor's active tagset (ISVISIBLE across all monitors).
 *   - Super+Tab (all_monitors=1): all windows on all tags, all monitors.
 *
 * Confirmation behaviour:
 *   - Always warps the pointer to the centre of the chosen window so that
 *     focus-follows-mouse takes effect immediately.
 *   - For Super+Tab, if the chosen window is on a hidden tag, calls view()
 *     to make that tag visible and seturgent() to highlight it in the bar,
 *     then focuses and warps.
 *
 * Thumbnail rendering (when compositor is active):
 *   1. Obtain the client's XComposite pixmap (cw->pixmap, or a fresh
 *      xcb_composite_name_window_pixmap if cw->pixmap is currently 0).
 *   2. Wrap the source pixmap in an XRender source picture.
 *   3. Allocate a destination pixmap at the desired thumbnail size and wrap
 *      it in an XRender destination picture.
 *   4. Set a projective scale transform on the source picture and call
 *      xcb_render_composite to perform server-side scaled copy.
 *   5. Wrap the destination pixmap in a cairo_xcb_surface and paint it in
 *      the GtkDrawingArea "draw" callback.
 *
 * Z-order / focus management:
 *   The switcher window uses override_redirect (set via GDK before the window
 *   is mapped) so awm does not manage it, does not steal focus from it, and
 *   does not restack it.  We raise it ourselves via xcb_configure_window on
 *   every show.
 *
 * See LICENSE file for copyright and license details. */

#ifdef COMPOSITOR

#include <assert.h>
#include <stdint.h>
#include <string.h>

#include <xcb/xcb.h>
#include <xcb/composite.h>
#include <xcb/render.h>
#include <xcb/xcb_renderutil.h>
#include <cairo/cairo.h>
#include <cairo/cairo-xcb.h>
#include <gtk/gtk.h>
#include <gdk/gdkx.h>

#include "awm.h"
#include "wmstate.h"
#include "client.h"
#include "log.h"
#include "compositor_backend.h"
#include "compositor.h"
#include "switcher.h"

/* -------------------------------------------------------------------------
 * Tunables
 * ---------------------------------------------------------------------- */

#define SW_MAX_THUMB_W 200 /* maximum thumbnail width  (px) */
#define SW_MAX_THUMB_H 150 /* maximum thumbnail height (px) */
#define SW_MIN_CARD_W 120  /* minimum card width       (px) */
#define SW_FALLBACK_W 120  /* card width when no thumbnail  */
#define SW_FALLBACK_H 80   /* card height when no thumbnail */
#define SW_CARD_PAD 8      /* padding inside each card      */
#define SW_CARD_GAP 6      /* gap between cards             */
#define SW_ICON_SIZE 24    /* icon size in the title row    */
#define SW_TITLE_H 36      /* height of the title row       */
#define SW_BORDER_W 3      /* selection highlight thickness */
#define SW_WIN_PAD 12      /* padding around the card row   */

/* -------------------------------------------------------------------------
 * Per-entry state (one per candidate window)
 * ---------------------------------------------------------------------- */

typedef struct {
	Client  *c;
	CompWin *cw; /* compositor window (may be NULL)   */
	/* Scaled thumbnail resources (NULL/0 if compositor unavailable) */
	xcb_pixmap_t         thumb_pixmap; /* destination pixmap at thumb size  */
	xcb_render_picture_t thumb_pict;   /* XRender picture on thumb_pixmap   */
	cairo_surface_t     *thumb_surf;   /* cairo_xcb_surface wrapping above  */
	int                  thumb_w;      /* actual rendered thumbnail width   */
	int                  thumb_h;      /* actual rendered thumbnail height  */
	int                  has_thumb;    /* 1 = thumbnail resources are valid */
	/* GTK card widget */
	GtkWidget *card; /* GtkDrawingArea                    */
} SwitcherEntry;

/* -------------------------------------------------------------------------
 * Module state
 * ---------------------------------------------------------------------- */

static GtkWidget *sw_win    = NULL;    /* persistent GTK window            */
static GtkWidget *sw_scroll = NULL;    /* GtkScrolledWindow                */
static GtkWidget *sw_box    = NULL;    /* GtkBox containing cards          */
static int        sw_active = 0;       /* 1 = switcher is visible          */
static int        sw_all_monitors = 0; /* 1 = show all monitors            */
static guint sw_refresh_id = 0; /* GLib timer for live thumbnail updates */

#define SW_REFRESH_MS 100 /* thumbnail refresh interval (ms) */

static SwitcherEntry *sw_entries  = NULL;
static int            sw_nentries = 0;
static int            sw_sel      = 0; /* index of selected entry          */

/* Forward declarations */
static void     switcher_hide(void);
static void     switcher_confirm(void);
static void     switcher_cancel(void);
static void     switcher_select(int idx);
static void     switcher_rebuild_cards(void);
static void     free_thumbnails(void);
static void     build_thumbnail(SwitcherEntry *e);
static void     refresh_thumbnail(SwitcherEntry *e);
static gboolean sw_refresh_cb(gpointer data);

/* -------------------------------------------------------------------------
 * XRender helpers
 * ---------------------------------------------------------------------- */

/* Find the XRender picture format for a given visual ID.
 * Returns 0 on failure. */
static xcb_render_pictformat_t
find_visual_format(xcb_visualid_t vid)
{
	const xcb_render_pictvisual_t *pv;

	if (!comp.render_formats)
		return 0;
	pv = xcb_render_util_find_visual_format(comp.render_formats, vid);
	return pv ? pv->format : 0;
}

/* Find the XRender picture format for a given depth.
 * Tries ARGB_32 for depth 32, RGB_24 for depth 24, else the root visual. */
static xcb_render_pictformat_t
find_format_for_depth(int depth)
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
	return find_visual_format(xcb_screen_root_visual(xc, screen));
}

/* Walk the xcb_screen visuals to find the xcb_visualtype_t for the root
 * visual of our screen. */
static xcb_visualtype_t *
get_root_visualtype(void)
{
	xcb_screen_iterator_t sit = xcb_setup_roots_iterator(xcb_get_setup(xc));
	for (int i = 0; i < screen; i++)
		xcb_screen_next(&sit);

	xcb_visualid_t       root_vid = sit.data->root_visual;
	xcb_depth_iterator_t dit = xcb_screen_allowed_depths_iterator(sit.data);
	for (; dit.rem; xcb_depth_next(&dit)) {
		xcb_visualtype_iterator_t vit = xcb_depth_visuals_iterator(dit.data);
		for (; vit.rem; xcb_visualtype_next(&vit)) {
			if (vit.data->visual_id == root_vid)
				return vit.data;
		}
	}
	return NULL;
}

/* Build the XRender thumbnail for entry e.
 * If cw->pixmap is 0 we acquire a fresh one ourselves and release it after
 * compositing (snapshot approach).
 * Leaves e->has_thumb = 0 on any error. */
static void
build_thumbnail(SwitcherEntry *e)
{
	CompWin                *cw;
	xcb_render_pictformat_t src_fmt, dst_fmt;
	xcb_render_picture_t    src_pict;
	xcb_render_transform_t  xform;
	double                  sx, sy, scale;
	int                     tw, th;
	xcb_void_cookie_t       ck;
	xcb_generic_error_t    *err;
	uint32_t                pict_mask  = 0;
	uint32_t                pict_val   = 0;
	xcb_pixmap_t            own_pixmap = 0; /* pixmap we acquired ourselves */

	e->has_thumb    = 0;
	e->thumb_pixmap = 0;
	e->thumb_pict   = 0;
	e->thumb_surf   = NULL;

	if (!comp.active)
		return;

	/* Find the compositor window for this client */
	for (cw = comp.windows; cw; cw = cw->next)
		if (cw->client == e->c)
			break;

	if (!cw || cw->w <= 0 || cw->h <= 0)
		return;

	e->cw = cw; /* remember for live refresh */

	/* Use cw->pixmap if available; otherwise acquire a fresh snapshot. */
	xcb_pixmap_t src_pixmap = cw->pixmap;
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
			return;
		}
		own_pixmap = pix;
		src_pixmap = pix;
	}

	/* Compute thumbnail dimensions, preserving aspect ratio */
	sx    = (double) SW_MAX_THUMB_W / (double) cw->w;
	sy    = (double) SW_MAX_THUMB_H / (double) cw->h;
	scale = sx < sy ? sx : sy;
	if (scale > 1.0)
		scale = 1.0;
	tw = (int) (cw->w * scale);
	th = (int) (cw->h * scale);
	if (tw < 1)
		tw = 1;
	if (th < 1)
		th = 1;

	/* Source picture wrapping the pixmap */
	src_fmt = find_format_for_depth(cw->depth);
	if (!src_fmt) {
		if (own_pixmap)
			xcb_free_pixmap(xc, own_pixmap);
		return;
	}

	src_pict = xcb_generate_id(xc);
	xcb_render_create_picture(xc, src_pict, (xcb_drawable_t) src_pixmap,
	    src_fmt, pict_mask, &pict_val);

	/* Set scale transform on the source picture.
	 * XRender uses a fixed-point 16.16 matrix.
	 * For scaling: element [0][0] = 1/scale_x, [1][1] = 1/scale_y,
	 * [2][2] = 1.0 (in 16.16: multiply by 65536). */
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

	/* Bilinear filter for nicer downscaling */
	{
		static const char filter[] = "good";
		xcb_render_set_picture_filter(
		    xc, src_pict, (uint16_t) (sizeof(filter) - 1), filter, 0, NULL);
	}

	/* Destination pixmap at thumbnail size (root depth / visual) */
	uint8_t dst_depth = (uint8_t) xcb_screen_root_depth(xc, screen);
	dst_fmt           = find_visual_format(xcb_screen_root_visual(xc, screen));
	if (!dst_fmt) {
		xcb_render_free_picture(xc, src_pict);
		if (own_pixmap)
			xcb_free_pixmap(xc, own_pixmap);
		return;
	}

	e->thumb_pixmap = xcb_generate_id(xc);
	ck              = xcb_create_pixmap_checked(xc, dst_depth, e->thumb_pixmap,
	                 (xcb_drawable_t) root, (uint16_t) tw, (uint16_t) th);
	xcb_flush(xc);
	err = xcb_request_check(xc, ck);
	if (err) {
		awm_warn("switcher: create thumb pixmap failed (error %d)",
		    (int) err->error_code);
		free(err);
		xcb_render_free_picture(xc, src_pict);
		if (own_pixmap)
			xcb_free_pixmap(xc, own_pixmap);
		e->thumb_pixmap = 0;
		return;
	}

	e->thumb_pict = xcb_generate_id(xc);
	xcb_render_create_picture(xc, e->thumb_pict,
	    (xcb_drawable_t) e->thumb_pixmap, dst_fmt, pict_mask, &pict_val);

	/* Pre-fill dst with opaque background so ARGB windows blend cleanly */
	{
		Clr               *bg = &scheme[SchemeNorm][ColBg];
		xcb_render_color_t bc;
		xcb_rectangle_t    br = { 0, 0, (uint16_t) tw, (uint16_t) th };
		bc.red                = bg->r;
		bc.green              = bg->g;
		bc.blue               = bg->b;
		bc.alpha              = 0xffff;
		xcb_render_fill_rectangles(
		    xc, XCB_RENDER_PICT_OP_SRC, e->thumb_pict, bc, 1, &br);
	}

	/* Composite: scale from source into destination.
	 * ARGB sources need OVER so pre-multiplied alpha blends against
	 * the background we just filled; opaque sources use SRC directly. */
	xcb_render_composite(xc,
	    cw->argb ? XCB_RENDER_PICT_OP_OVER : XCB_RENDER_PICT_OP_SRC, src_pict,
	    XCB_RENDER_PICTURE_NONE, e->thumb_pict, 0, 0, /* src x,y  */
	    0, 0,                                         /* mask x,y */
	    0, 0,                                         /* dst x,y  */
	    (uint16_t) tw, (uint16_t) th);
	xcb_flush(xc);

	xcb_render_free_picture(xc, src_pict);
	/* Release snapshot pixmap we acquired ourselves (if any) */
	if (own_pixmap)
		xcb_free_pixmap(xc, own_pixmap);

	/* Wrap the destination pixmap as a cairo surface */
	{
		xcb_visualtype_t *vis = get_root_visualtype();
		if (!vis) {
			xcb_render_free_picture(xc, e->thumb_pict);
			xcb_free_pixmap(xc, e->thumb_pixmap);
			e->thumb_pict   = 0;
			e->thumb_pixmap = 0;
			return;
		}

		e->thumb_surf = cairo_xcb_surface_create(
		    xc, (xcb_drawable_t) e->thumb_pixmap, vis, tw, th);
		if (!e->thumb_surf ||
		    cairo_surface_status(e->thumb_surf) != CAIRO_STATUS_SUCCESS) {
			if (e->thumb_surf) {
				cairo_surface_destroy(e->thumb_surf);
				e->thumb_surf = NULL;
			}
			xcb_render_free_picture(xc, e->thumb_pict);
			xcb_free_pixmap(xc, e->thumb_pixmap);
			e->thumb_pict   = 0;
			e->thumb_pixmap = 0;
			return;
		}
	}

	e->thumb_w   = tw;
	e->thumb_h   = th;
	e->has_thumb = 1;
}

/* Re-composite the thumbnail from the live compositor pixmap.
 * Called periodically by the refresh timer while the switcher is open.
 * Reuses the existing destination pixmap/picture/surface if the window
 * size has not changed; rebuilds them if it has (or if has_thumb is 0).
 *
 * Uses comp_capture_thumb() (GL FBO + glReadPixels) to get a fresh cairo
 * image surface from the live GL texture.  This is the only path that
 * returns current content on the EGL compositor backend. */
static void
refresh_thumbnail(SwitcherEntry *e)
{
	cairo_surface_t *surf;

	if (!e->has_thumb || !e->c)
		return;

	/* Capture a fresh frame from the live GL texture */
	surf = comp_capture_thumb(e->c, SW_MAX_THUMB_W, SW_MAX_THUMB_H);
	if (!surf)
		return;

	/* Swap in the new surface, release the old one */
	if (e->thumb_surf)
		cairo_surface_destroy(e->thumb_surf);
	e->thumb_surf = surf;

	/* Update dimensions in case the window was resized */
	e->thumb_w = cairo_image_surface_get_width(surf);
	e->thumb_h = cairo_image_surface_get_height(surf);
}

/* GLib timer callback — refresh all thumbnails and queue redraws */
static gboolean
sw_refresh_cb(gpointer data)
{
	(void) data;
	if (!sw_active)
		return G_SOURCE_REMOVE;

	for (int i = 0; i < sw_nentries; i++) {
		refresh_thumbnail(&sw_entries[i]);
		if (sw_entries[i].card)
			gtk_widget_queue_draw(sw_entries[i].card);
	}
	return G_SOURCE_CONTINUE;
}

/* -------------------------------------------------------------------------
 * Card dimensions
 * ---------------------------------------------------------------------- */

/* Return card width for entry e (including padding). */
static int
card_w(const SwitcherEntry *e)
{
	if (e->has_thumb) {
		int w = e->thumb_w + 2 * SW_CARD_PAD;
		if (w < SW_MIN_CARD_W)
			w = SW_MIN_CARD_W;
		return w;
	}
	return SW_FALLBACK_W;
}

/* Return card height for entry e. */
static int
card_h(const SwitcherEntry *e)
{
	if (e->has_thumb)
		return e->thumb_h + 2 * SW_CARD_PAD + SW_TITLE_H;
	return SW_FALLBACK_H;
}

/* -------------------------------------------------------------------------
 * GTK card draw callback
 * ---------------------------------------------------------------------- */

static gboolean
on_card_draw(GtkWidget *widget, cairo_t *cr, gpointer data)
{
	SwitcherEntry *e   = (SwitcherEntry *) data;
	int            idx = -1;
	int            w   = gtk_widget_get_allocated_width(widget);
	int            h   = gtk_widget_get_allocated_height(widget);

	assert(e != NULL);
	assert(e->c != NULL);

	/* Find our index */
	for (int i = 0; i < sw_nentries; i++) {
		if (sw_entries[i].card == widget) {
			idx = i;
			break;
		}
	}

	int selected = (idx == sw_sel);

	/* Background */
	if (selected) {
		Clr *bg = &scheme[SchemeSel][ColBg];
		cairo_set_source_rgba(cr, (double) bg->r / 65535.0,
		    (double) bg->g / 65535.0, (double) bg->b / 65535.0, 1.0);
	} else {
		Clr *bg = &scheme[SchemeNorm][ColBg];
		cairo_set_source_rgba(cr, (double) bg->r / 65535.0,
		    (double) bg->g / 65535.0, (double) bg->b / 65535.0, 0.90);
	}
	cairo_rectangle(cr, 0, 0, w, h);
	cairo_fill(cr);

	/* Thumbnail or placeholder */
	if (e->has_thumb && e->thumb_surf) {
		int tx = (w - e->thumb_w) / 2;
		int ty = SW_CARD_PAD;
		cairo_save(cr);
		cairo_set_source_surface(cr, e->thumb_surf, tx, ty);
		cairo_rectangle(cr, tx, ty, e->thumb_w, e->thumb_h);
		cairo_fill(cr);
		cairo_restore(cr);
	} else {
		/* Placeholder: dim rect where thumbnail would be */
		int ph = h - SW_TITLE_H - 2 * SW_CARD_PAD;
		if (ph > 0) {
			cairo_set_source_rgba(cr, 0.1, 0.1, 0.1, 0.5);
			cairo_rectangle(
			    cr, SW_CARD_PAD, SW_CARD_PAD, w - 2 * SW_CARD_PAD, ph);
			cairo_fill(cr);
		}
	}

	/* Title row — bottom of card */
	int title_y = h - SW_TITLE_H;

	/* Separator line above title */
	{
		cairo_set_source_rgba(cr, 0.3, 0.3, 0.3, 0.6);
		cairo_set_line_width(cr, 1.0);
		cairo_move_to(cr, 0, title_y);
		cairo_line_to(cr, w, title_y);
		cairo_stroke(cr);
	}

	/* Icon */
	int icon_drawn_w = 0;
	if (e->c->icon) {
		cairo_surface_type_t stype      = cairo_surface_get_type(e->c->icon);
		int                  icon_src_w = 0, icon_src_h = 0;
		if (stype == CAIRO_SURFACE_TYPE_IMAGE) {
			icon_src_w = cairo_image_surface_get_width(e->c->icon);
			icon_src_h = cairo_image_surface_get_height(e->c->icon);
		}
		if (icon_src_w > 0 && icon_src_h > 0) {
			int icon_x = SW_CARD_PAD;
			int icon_y = title_y + (SW_TITLE_H - SW_ICON_SIZE) / 2;
			cairo_save(cr);
			cairo_translate(cr, icon_x, icon_y);
			double larger = icon_src_w > icon_src_h ? (double) icon_src_w
			                                        : (double) icon_src_h;
			double iscale = (double) SW_ICON_SIZE / larger;
			cairo_scale(cr, iscale, iscale);
			cairo_set_source_surface(cr, e->c->icon, 0, 0);
			cairo_paint(cr);
			cairo_restore(cr);
			icon_drawn_w = SW_ICON_SIZE + 4;
		}
	}

	/* Window title text */
	{
		Clr *fg =
		    selected ? &scheme[SchemeSel][ColFg] : &scheme[SchemeNorm][ColFg];
		cairo_set_source_rgba(cr, (double) fg->r / 65535.0,
		    (double) fg->g / 65535.0, (double) fg->b / 65535.0, 1.0);

		int txt_x = SW_CARD_PAD + icon_drawn_w;
		int txt_w = w - txt_x - SW_CARD_PAD;
		if (txt_w > 0 && drw && drw->fonts && drw->fonts->desc) {
			PangoLayout *layout = pango_cairo_create_layout(cr);
			pango_layout_set_font_description(layout, drw->fonts->desc);
			pango_layout_set_text(layout, e->c->name, -1);
			pango_layout_set_width(layout, txt_w * PANGO_SCALE);
			pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
			pango_layout_set_single_paragraph_mode(layout, TRUE);

			/* Centre text vertically in title row */
			int pw = 0, ph = 0;
			pango_layout_get_pixel_size(layout, &pw, &ph);
			int txt_y = title_y + (SW_TITLE_H - ph) / 2;
			if (txt_y < title_y)
				txt_y = title_y;

			cairo_move_to(cr, txt_x, txt_y);
			pango_cairo_show_layout(cr, layout);
			g_object_unref(layout);
		}
	}

	/* Selection border */
	if (selected) {
		Clr *border = &scheme[SchemeSel][ColBorder];
		cairo_set_source_rgba(cr, (double) border->r / 65535.0,
		    (double) border->g / 65535.0, (double) border->b / 65535.0, 1.0);
		cairo_set_line_width(cr, SW_BORDER_W);
		cairo_rectangle(cr, SW_BORDER_W / 2.0, SW_BORDER_W / 2.0,
		    w - SW_BORDER_W, h - SW_BORDER_W);
		cairo_stroke(cr);
	}

	return FALSE;
}

/* -------------------------------------------------------------------------
 * Free thumbnails
 * ---------------------------------------------------------------------- */

static void
free_thumbnails(void)
{
	for (int i = 0; i < sw_nentries; i++) {
		SwitcherEntry *e = &sw_entries[i];
		if (e->thumb_surf) {
			cairo_surface_destroy(e->thumb_surf);
			e->thumb_surf = NULL;
		}
		if (e->thumb_pict) {
			xcb_render_free_picture(xc, e->thumb_pict);
			e->thumb_pict = 0;
		}
		if (e->thumb_pixmap) {
			xcb_free_pixmap(xc, e->thumb_pixmap);
			e->thumb_pixmap = 0;
		}
		e->has_thumb = 0;
	}
}

/* -------------------------------------------------------------------------
 * Rebuild card widgets
 * Called after sw_entries[] is populated.
 * ---------------------------------------------------------------------- */

static void
switcher_rebuild_cards(void)
{
	/* Remove all existing children */
	GList *children = gtk_container_get_children(GTK_CONTAINER(sw_box));
	for (GList *l = children; l; l = l->next)
		gtk_container_remove(GTK_CONTAINER(sw_box), GTK_WIDGET(l->data));
	g_list_free(children);

	for (int i = 0; i < sw_nentries; i++) {
		SwitcherEntry *e     = &sw_entries[i];
		int            cw_px = card_w(e);
		int            ch_px = card_h(e);

		GtkWidget *da = gtk_drawing_area_new();
		gtk_widget_set_size_request(da, cw_px, ch_px);
		g_signal_connect(da, "draw", G_CALLBACK(on_card_draw), e);
		gtk_box_pack_start(GTK_BOX(sw_box), da, FALSE, FALSE, SW_CARD_GAP / 2);
		e->card = da;
	}

	gtk_widget_show_all(sw_box);
}

/* -------------------------------------------------------------------------
 * Selection helper
 * ---------------------------------------------------------------------- */

static void
switcher_select(int idx)
{
	if (sw_nentries <= 0)
		return;
	/* Wrap */
	if (idx < 0)
		idx = sw_nentries - 1;
	if (idx >= sw_nentries)
		idx = 0;

	assert(sw_sel >= 0 || sw_nentries == 0);
	assert(idx >= 0 && idx < sw_nentries);
	int old = sw_sel;
	sw_sel  = idx;

	/* Redraw old and new cards */
	if (old >= 0 && old < sw_nentries && sw_entries[old].card)
		gtk_widget_queue_draw(sw_entries[old].card);
	if (sw_entries[sw_sel].card)
		gtk_widget_queue_draw(sw_entries[sw_sel].card);

	/* Scroll selected card into view */
	if (sw_entries[sw_sel].card) {
		GtkAdjustment *hadj = gtk_scrolled_window_get_hadjustment(
		    GTK_SCROLLED_WINDOW(sw_scroll));
		if (hadj) {
			GtkAllocation alloc;
			gtk_widget_get_allocation(sw_entries[sw_sel].card, &alloc);
			double lo = gtk_adjustment_get_value(hadj);
			double pg = gtk_adjustment_get_page_size(hadj);
			if (alloc.x < lo)
				gtk_adjustment_set_value(hadj, alloc.x);
			else if (alloc.x + alloc.width > lo + pg)
				gtk_adjustment_set_value(hadj, alloc.x + alloc.width - pg);
		}
	}
}

/* -------------------------------------------------------------------------
 * Show / hide / confirm / cancel
 * ---------------------------------------------------------------------- */

static void
switcher_hide(void)
{
	/* Stop the live thumbnail refresh timer */
	if (sw_refresh_id) {
		g_source_remove(sw_refresh_id);
		sw_refresh_id = 0;
	}

	free_thumbnails();

	if (sw_entries) {
		free(sw_entries);
		sw_entries  = NULL;
		sw_nentries = 0;
	}

	/* Release the keyboard grab we took in switcher_show_internal */
	xcb_ungrab_keyboard(xc, XCB_CURRENT_TIME);

	sw_active = 0;
	if (sw_win)
		gtk_widget_hide(sw_win);
	xcb_flush(xc);
}

static void
switcher_confirm(void)
{
	Client *chosen =
	    (sw_sel >= 0 && sw_sel < sw_nentries) ? sw_entries[sw_sel].c : NULL;
	int all = sw_all_monitors;
	switcher_hide();
	if (!chosen)
		return;

	assert(chosen->mon != NULL);
	if (all && !ISVISIBLE(chosen, chosen->mon)) {
		/* Super+Tab and window is on a hidden tag: make the tag visible.
		 * Switch selmon to the monitor that owns the window first, then
		 * call view() on the window's tag so it becomes visible. */
		g_awm_set_selmon(chosen->mon);
		Arg a = { .ui = chosen->tags };
		view(&a);
		/* seturgent so the bar highlights the tag */
		seturgent(chosen, 1);
	}

	focus(chosen);
	/* Warp the pointer to the centre of the chosen window unconditionally.
	 * This is required for focus-follows-mouse: without a warp the pointer
	 * stays where it is and the next mouse-move will steal focus back. */
	xcb_warp_pointer(xc, XCB_WINDOW_NONE, chosen->win, 0, 0, 0, 0,
	    (int16_t) (chosen->w / 2), (int16_t) (chosen->h / 2));
	xcb_flush(xc);
}

static void
switcher_cancel(void)
{
	switcher_hide();
}

/* -------------------------------------------------------------------------
 * Internal show helper
 * ---------------------------------------------------------------------- */

static void
switcher_show_internal(int all_monitors, int start_prev)
{
	if (!sw_win)
		return;

	/* If already showing, ignore (caller should use switcher_next/prev) */
	if (sw_active)
		return;

	sw_all_monitors = all_monitors;

	/* ------------------------------------------------------------------ *
	 * Collect candidate clients
	 * ------------------------------------------------------------------ */
	int capacity = 64;
	sw_entries =
	    (SwitcherEntry *) calloc((size_t) capacity, sizeof(SwitcherEntry));
	if (!sw_entries)
		return;
	sw_nentries = 0;

	for (Client *c = g_awm.clients_head; c; c = c->next) {
		if (c->ishidden || c->issni)
			continue;
		/* all_monitors=1 (Super+Tab): every window on every tag.
		 * all_monitors=0 (Alt+Tab):   every window currently visible
		 *   on any monitor's active tagset (across all monitors). */
		if (!all_monitors && !ISVISIBLE(c, c->mon))
			continue;

		if (sw_nentries >= capacity) {
			capacity *= 2;
			SwitcherEntry *tmp = (SwitcherEntry *) realloc(
			    sw_entries, (size_t) capacity * sizeof(SwitcherEntry));
			if (!tmp) {
				free(sw_entries);
				sw_entries  = NULL;
				sw_nentries = 0;
				return;
			}
			sw_entries = tmp;
		}
		SwitcherEntry *e = &sw_entries[sw_nentries++];
		memset(e, 0, sizeof(*e));
		e->c = c;
	}

	if (sw_nentries == 0) {
		free(sw_entries);
		sw_entries = NULL;
		return;
	}

	/* Start selection: index 1 (next after current) for forward, or
	 * (n-1) for backward.  Clamp for single-window case. */
	if (start_prev)
		sw_sel = (sw_nentries > 1) ? sw_nentries - 1 : 0;
	else
		sw_sel = (sw_nentries > 1) ? 1 : 0;

	/* Build thumbnails */
	for (int i = 0; i < sw_nentries; i++)
		build_thumbnail(&sw_entries[i]);

	/* Build card widgets */
	switcher_rebuild_cards();

	/* ------------------------------------------------------------------ *
	 * Size and position the window
	 * ------------------------------------------------------------------ */
	{
		int total_w = 2 * SW_WIN_PAD;
		int max_h   = SW_FALLBACK_H;

		for (int i = 0; i < sw_nentries; i++) {
			total_w += card_w(&sw_entries[i]) + SW_CARD_GAP;
			int ch = card_h(&sw_entries[i]);
			if (ch > max_h)
				max_h = ch;
		}
		total_w += SW_WIN_PAD;

		assert(g_awm.selmon_num >= 0 &&
		    g_awm.selmon_num < (int) g_awm.n_monitors);
		Monitor *m = g_awm_selmon;
		if (total_w > m->ww)
			total_w = m->ww;

		int win_h = max_h + 2 * SW_WIN_PAD;
		int win_x = m->wx + (m->ww - total_w) / 2;
		int win_y = m->wy + (m->wh - win_h) / 2;

		gtk_window_resize(GTK_WINDOW(sw_win), total_w, win_h);
		gtk_window_move(GTK_WINDOW(sw_win), win_x, win_y);
	}

	sw_active = 1;
	gtk_widget_show_all(sw_win);

	/* Start the live thumbnail refresh timer */
	sw_refresh_id = g_timeout_add(SW_REFRESH_MS, sw_refresh_cb, NULL);

	/* Process pending GTK events so the window gets mapped and drawn
	 * before we return to the main loop. */
	while (gtk_events_pending())
		gtk_main_iteration_do(FALSE);

	/* Raise the switcher window to the top of the stack via XCB.
	 * We do this after GTK has mapped it so the XID is valid. */
	{
		GdkWindow *gwin = gtk_widget_get_window(sw_win);
		if (gwin) {
			xcb_window_t xwin = (xcb_window_t) gdk_x11_window_get_xid(gwin);
			if (xwin) {
				uint32_t stack_above = XCB_STACK_MODE_ABOVE;
				xcb_configure_window(
				    xc, xwin, XCB_CONFIG_WINDOW_STACK_MODE, &stack_above);
				xcb_flush(xc);
			}
		}
	}

	/* Grab the keyboard so we receive all key events (including releases of
	 * Alt/Super) while the switcher is open.  This supplements the passive
	 * grabs on individual keybindings. */
	xcb_grab_keyboard(xc, 0, root, XCB_CURRENT_TIME, XCB_GRAB_MODE_ASYNC,
	    XCB_GRAB_MODE_ASYNC);
	xcb_flush(xc);
}

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

void
switcher_init(void)
{
	if (sw_win)
		return;

	sw_win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
	gtk_window_set_title(GTK_WINDOW(sw_win), "awm-switcher");
	gtk_window_set_decorated(GTK_WINDOW(sw_win), FALSE);
	gtk_window_set_skip_taskbar_hint(GTK_WINDOW(sw_win), TRUE);
	gtk_window_set_skip_pager_hint(GTK_WINDOW(sw_win), TRUE);
	gtk_window_set_type_hint(GTK_WINDOW(sw_win), GDK_WINDOW_TYPE_HINT_DIALOG);

	/* Semi-transparent background via RGBA visual if available */
	{
		GdkScreen *gscreen = gtk_widget_get_screen(sw_win);
		GdkVisual *vis     = gdk_screen_get_rgba_visual(gscreen);
		if (vis)
			gtk_widget_set_visual(sw_win, vis);
		gtk_widget_set_app_paintable(sw_win, TRUE);
	}

	/* WM_CLASS so applyrules() could match (harmless for override_redirect) */
	gtk_window_set_wmclass(GTK_WINDOW(sw_win), "awm-switcher", "awm-switcher");

	/* Override-redirect: awm will not manage this window.
	 * Must be set before the window is realized. */
	gtk_window_set_type_hint(
	    GTK_WINDOW(sw_win), GDK_WINDOW_TYPE_HINT_POPUP_MENU);
	{
		/* Realize now so we can set override_redirect on the GdkWindow */
		gtk_widget_realize(sw_win);
		GdkWindow *gwin = gtk_widget_get_window(sw_win);
		if (gwin)
			gdk_window_set_override_redirect(gwin, TRUE);
	}

	/* Container layout */
	GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_container_set_border_width(GTK_CONTAINER(outer), SW_WIN_PAD);
	gtk_container_add(GTK_CONTAINER(sw_win), outer);

	sw_scroll = gtk_scrolled_window_new(NULL, NULL);
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw_scroll),
	    GTK_POLICY_AUTOMATIC, GTK_POLICY_NEVER);
	gtk_box_pack_start(GTK_BOX(outer), sw_scroll, FALSE, FALSE, 0);

	sw_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	gtk_container_set_border_width(GTK_CONTAINER(sw_box), 0);
	gtk_container_add(GTK_CONTAINER(sw_scroll), sw_box);

	/* Intercept window close button */
	g_signal_connect(
	    sw_win, "delete-event", G_CALLBACK(gtk_widget_hide_on_delete), NULL);

	gtk_widget_hide(sw_win);
}

void
switcher_show(const Arg *arg)
{
	int all_monitors = arg ? arg->i : 0;
	if (sw_active) {
		switcher_next(NULL);
		return;
	}
	switcher_show_internal(all_monitors, 0);
}

void
switcher_show_prev(const Arg *arg)
{
	int all_monitors = arg ? arg->i : 0;
	if (sw_active) {
		switcher_prev(NULL);
		return;
	}
	switcher_show_internal(all_monitors, 1);
}

void
switcher_next(const Arg *arg)
{
	(void) arg;
	if (!sw_active)
		return;
	switcher_select(sw_sel + 1);
	/* Flush GTK redraws immediately */
	while (gtk_events_pending())
		gtk_main_iteration_do(FALSE);
}

void
switcher_prev(const Arg *arg)
{
	(void) arg;
	if (!sw_active)
		return;
	switcher_select(sw_sel - 1);
	while (gtk_events_pending())
		gtk_main_iteration_do(FALSE);
}

void
switcher_confirm_xkb(const Arg *arg)
{
	(void) arg;
	if (!sw_active)
		return;
	switcher_confirm();
}

void
switcher_cancel_xkb(const Arg *arg)
{
	(void) arg;
	if (!sw_active)
		return;
	switcher_cancel();
}

void
switcher_cleanup(void)
{
	free_thumbnails();
	if (sw_entries) {
		free(sw_entries);
		sw_entries  = NULL;
		sw_nentries = 0;
	}
	if (sw_win) {
		gtk_widget_destroy(sw_win);
		sw_win    = NULL;
		sw_scroll = NULL;
		sw_box    = NULL;
	}
	sw_active = 0;
}

int
switcher_active(void)
{
	return sw_active;
}

#else /* !COMPOSITOR */

/* Stub implementations when compositor is disabled */
#include "awm.h"
#include "switcher.h"

void
switcher_init(void)
{
}
void
switcher_show(const Arg *arg)
{
	(void) arg;
}
void
switcher_show_prev(const Arg *arg)
{
	(void) arg;
}
void
switcher_next(const Arg *arg)
{
	(void) arg;
}
void
switcher_prev(const Arg *arg)
{
	(void) arg;
}
void
switcher_confirm_xkb(const Arg *arg)
{
	(void) arg;
}
void
switcher_cancel_xkb(const Arg *arg)
{
	(void) arg;
}
void
switcher_cleanup(void)
{
}
int
switcher_active(void)
{
	return 0;
}

#endif /* COMPOSITOR */
