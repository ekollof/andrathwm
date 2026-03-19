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
 *   Delegates to comp_capture_thumb() which dispatches through the
 *   compositor backend vtable (GL FBO + glReadPixels for EGL; xcb_get_image
 *   via XRender for the XRender backend).  The switcher holds no X pixmap or
 *   XRender picture resources of its own.  Thumbnails are refreshed on a
 *   100 ms g_timeout_add timer by calling comp_capture_thumb() again in
 *   refresh_thumbnail().
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
	cairo_surface_t
	    *thumb_surf; /* cairo image surface from comp_capture_thumb */
	int  thumb_w;    /* actual rendered thumbnail width   */
	int  thumb_h;    /* actual rendered thumbnail height  */
	int  has_thumb;  /* 1 = thumbnail resources are valid */
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
static void     refresh_thumbnail(SwitcherEntry *e);
static gboolean sw_refresh_cb(gpointer data);

/* -------------------------------------------------------------------------
 * Initial thumbnail capture
 * Delegates to comp_capture_thumb() (GL FBO path for EGL, XRender path for
 * XRender backend).  Sets e->has_thumb = 1 on success.
 * ---------------------------------------------------------------------- */

static void
build_thumbnail(SwitcherEntry *e)
{
	cairo_surface_t *surf;

	e->has_thumb  = 0;
	e->thumb_surf = NULL;

	if (!comp.active || !e->c)
		return;

	surf = comp_capture_thumb(e->c, SW_MAX_THUMB_W, SW_MAX_THUMB_H);
	if (!surf)
		return;

	e->thumb_surf = surf;
	e->thumb_w    = cairo_image_surface_get_width(surf);
	e->thumb_h    = cairo_image_surface_get_height(surf);
	e->has_thumb  = 1;
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
		if (txt_w > 0 && drw && render_surface_font_desc(drw)) {
			PangoLayout *layout = pango_cairo_create_layout(cr);
			pango_layout_set_font_description(
			    layout, render_surface_font_desc(drw));
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
	g_wm_backend->ungrab_keyboard(&g_plat, XCB_CURRENT_TIME);

	sw_active = 0;
	if (sw_win)
		gtk_widget_hide(sw_win);
	g_wm_backend->flush(&g_plat);
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
	g_wm_backend->warp_pointer(
	    &g_plat, chosen->win, chosen->w / 2, chosen->h / 2);
	g_wm_backend->flush(&g_plat);
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
			WinId xwin = (WinId) gdk_x11_window_get_xid(gwin);
			if (xwin) {
				uint32_t stack_above = AWM_STACK_MODE_ABOVE;
				g_wm_backend->configure_win(
				    &g_plat, xwin, AWM_CONFIG_WIN_STACK_MODE, &stack_above);
				g_wm_backend->flush(&g_plat);
			}
		}
	}

	/* Grab the keyboard so we receive all key events (including releases of
	 * Alt/Super) while the switcher is open.  This supplements the passive
	 * grabs on individual keybindings. */
	g_wm_backend->grab_keyboard(&g_plat, g_plat.root, XCB_CURRENT_TIME);
	g_wm_backend->flush(&g_plat);
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
