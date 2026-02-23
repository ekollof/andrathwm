/* switcher.c — Alt+Tab / Super+Tab window switcher for awm
 *
 * Architecture:
 *   - A persistent (initially hidden) GTK floating window is created once in
 *     switcher_init() and reused on every invocation.
 *   - switcher_show() collects the current client list, builds XRender
 *     pre-scaled thumbnail pixmaps for each client, populates a row of
 *     GtkDrawingArea cards inside a GtkScrolledWindow, and makes the window
 *     visible.
 *   - The user cycles with Tab / Shift+Tab (ISO_Left_Tab); Escape cancels,
 *     Return or release of Alt/Super confirms.
 *   - On hide, all thumbnail pixmaps/pictures are freed.
 *
 * Thumbnail rendering (when compositor is active and cw->ever_damaged):
 *   1. Wrap the client's XComposite pixmap in an XRender source picture.
 *   2. Allocate a destination pixmap at the desired thumbnail size and wrap
 *      it in an XRender destination picture.
 *   3. Set a projective scale transform on the source picture and call
 *      xcb_render_composite to perform server-side scaled copy.
 *   4. Wrap the destination pixmap in a cairo_xcb_surface and paint it in
 *      the GtkDrawingArea "draw" callback.
 *
 * See LICENSE file for copyright and license details. */

#ifdef COMPOSITOR

#include <stdint.h>
#include <string.h>

#include <xcb/xcb.h>
#include <xcb/render.h>
#include <xcb/xcb_renderutil.h>
#include <cairo/cairo.h>
#include <cairo/cairo-xcb.h>
#include <gtk/gtk.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#include "awm.h"
#include "client.h"
#include "log.h"
#include "compositor_backend.h"
#include "switcher.h"

/* -------------------------------------------------------------------------
 * Tunables
 * ---------------------------------------------------------------------- */

#define SW_MAX_THUMB_W 200 /* maximum thumbnail width  (px) */
#define SW_MAX_THUMB_H 150 /* maximum thumbnail height (px) */
#define SW_MIN_CARD_W 100  /* minimum card width       (px) */
#define SW_FALLBACK_W 100  /* card width when no thumbnail  */
#define SW_FALLBACK_H 80   /* card height when no thumbnail */
#define SW_CARD_PAD 8      /* padding inside each card      */
#define SW_CARD_GAP 6      /* gap between cards             */
#define SW_ICON_SIZE 24    /* icon size in the title row    */
#define SW_TITLE_H 32      /* height of the title row       */
#define SW_BORDER_W 2      /* selection highlight thickness */
#define SW_WIN_PAD 12      /* padding around the card row   */

/* -------------------------------------------------------------------------
 * Per-entry state (one per candidate window)
 * ---------------------------------------------------------------------- */

typedef struct {
	Client *c;
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
static int        sw_all_monitors = 0; /* 1 = show all monitors        */

static SwitcherEntry *sw_entries  = NULL;
static int            sw_nentries = 0;
static int            sw_sel      = 0; /* index of selected entry          */

/* Forward declarations */
static void switcher_hide(void);
static void switcher_confirm(void);
static void switcher_cancel(void);
static void switcher_select(int idx);
static void switcher_rebuild_cards(void);
static void free_thumbnails(void);

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

/* Build the XRender thumbnail for entry e.
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
	uint32_t                pict_mask;
	uint32_t                pict_val = 0;

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

	if (!cw || !cw->ever_damaged || !cw->pixmap || cw->w <= 0 || cw->h <= 0)
		return;

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

	/* Source picture wrapping the composite pixmap */
	src_fmt = find_format_for_depth(cw->depth);
	if (!src_fmt)
		return;

	src_pict  = xcb_generate_id(xc);
	pict_mask = 0;
	xcb_render_create_picture(xc, src_pict, (xcb_drawable_t) cw->pixmap,
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
		/* "good" filter string */
		static const char filter[] = "good";
		xcb_render_set_picture_filter(
		    xc, src_pict, (uint16_t) (sizeof(filter) - 1), filter, 0, NULL);
	}

	/* Destination pixmap at thumbnail size */
	uint8_t dst_depth = (uint8_t) xcb_screen_root_depth(xc, screen);
	dst_fmt           = find_visual_format(xcb_screen_root_visual(xc, screen));
	if (!dst_fmt) {
		xcb_render_free_picture(xc, src_pict);
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
		e->thumb_pixmap = 0;
		return;
	}

	e->thumb_pict = xcb_generate_id(xc);
	pict_mask     = 0;
	xcb_render_create_picture(xc, e->thumb_pict,
	    (xcb_drawable_t) e->thumb_pixmap, dst_fmt, pict_mask, &pict_val);

	/* Composite: scale from source into destination */
	xcb_render_composite(xc, XCB_RENDER_PICT_OP_SRC, src_pict,
	    XCB_RENDER_PICTURE_NONE, e->thumb_pict, 0, 0, /* src x,y */
	    0, 0,                                         /* mask x,y */
	    0, 0,                                         /* dst x,y */
	    (uint16_t) tw, (uint16_t) th);
	xcb_flush(xc);

	xcb_render_free_picture(xc, src_pict);

	/* Wrap the destination pixmap as a cairo surface */
	{
		xcb_screen_iterator_t sit =
		    xcb_setup_roots_iterator(xcb_get_setup(xc));
		int i;
		for (i = 0; i < screen; i++)
			xcb_screen_next(&sit);

		xcb_visualtype_t    *vis = NULL;
		xcb_depth_iterator_t dit =
		    xcb_screen_allowed_depths_iterator(sit.data);
		for (; dit.rem; xcb_depth_next(&dit)) {
			if (dit.data->depth != dst_depth)
				continue;
			xcb_visualtype_iterator_t vit =
			    xcb_depth_visuals_iterator(dit.data);
			for (; vit.rem; xcb_visualtype_next(&vit)) {
				if (vit.data->visual_id ==
				    xcb_screen_root_visual(xc, screen)) {
					vis = vit.data;
					break;
				}
			}
			if (vis)
				break;
		}
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
		/* selection highlight: use selbgcolor-derived colour */
		Clr *bg = &scheme[SchemeSel][ColBg];
		cairo_set_source_rgba(cr, (double) bg->r / 65535.0,
		    (double) bg->g / 65535.0, (double) bg->b / 65535.0, 1.0);
	} else {
		Clr *bg = &scheme[SchemeNorm][ColBg];
		cairo_set_source_rgba(cr, (double) bg->r / 65535.0,
		    (double) bg->g / 65535.0, (double) bg->b / 65535.0, 0.85);
	}
	cairo_rectangle(cr, 0, 0, w, h);
	cairo_fill(cr);

	/* Thumbnail or placeholder */
	if (e->has_thumb && e->thumb_surf) {
		int tx = (w - e->thumb_w) / 2;
		int ty = SW_CARD_PAD;
		cairo_set_source_surface(cr, e->thumb_surf, tx, ty);
		cairo_paint(cr);
	} else {
		/* Placeholder: dim rect where thumbnail would be */
		int ph = h - SW_TITLE_H - 2 * SW_CARD_PAD;
		if (ph < 0)
			ph = 0;
		cairo_set_source_rgba(cr, 0.1, 0.1, 0.1, 0.5);
		cairo_rectangle(cr, SW_CARD_PAD, SW_CARD_PAD, w - 2 * SW_CARD_PAD, ph);
		cairo_fill(cr);
	}

	/* Title row — bottom of card */
	int title_y = h - SW_TITLE_H;

	/* Icon */
	if (e->c->icon) {
		int icon_x = SW_CARD_PAD;
		int icon_y = title_y + (SW_TITLE_H - SW_ICON_SIZE) / 2;
		cairo_save(cr);
		cairo_translate(cr, icon_x, icon_y);
		double icon_src_w = cairo_image_surface_get_width(e->c->icon);
		double icon_src_h = cairo_image_surface_get_height(e->c->icon);
		if (icon_src_w > 0 && icon_src_h > 0) {
			double iscale = (double) SW_ICON_SIZE /
			    (icon_src_w > icon_src_h ? icon_src_w : icon_src_h);
			cairo_scale(cr, iscale, iscale);
		}
		cairo_set_source_surface(cr, e->c->icon, 0, 0);
		cairo_paint(cr);
		cairo_restore(cr);
	}

	/* Window title text */
	{
		Clr *fg =
		    selected ? &scheme[SchemeSel][ColFg] : &scheme[SchemeNorm][ColFg];
		cairo_set_source_rgba(cr, (double) fg->r / 65535.0,
		    (double) fg->g / 65535.0, (double) fg->b / 65535.0, 1.0);

		int txt_x = SW_CARD_PAD + (e->c->icon ? SW_ICON_SIZE + 4 : 0);
		int txt_w = w - txt_x - SW_CARD_PAD;
		if (txt_w > 0 && drw && drw->fonts && drw->fonts->desc) {
			PangoLayout *layout = pango_cairo_create_layout(cr);
			pango_layout_set_font_description(layout, drw->fonts->desc);
			pango_layout_set_text(layout, e->c->name, -1);
			pango_layout_set_width(layout, txt_w * PANGO_SCALE);
			pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
			pango_layout_set_single_paragraph_mode(layout, TRUE);
			int txt_y = title_y + (SW_TITLE_H - (int) drw->fonts->h) / 2;
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

	return FALSE; /* let GTK draw children if any */
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
 * Key event handlers
 * ---------------------------------------------------------------------- */

static gboolean
on_key_press(GtkWidget *widget, GdkEventKey *ev, gpointer data)
{
	(void) widget;
	(void) data;

	guint ks = ev->keyval;

	if (ks == GDK_KEY_Escape) {
		switcher_cancel();
		return TRUE;
	}
	if (ks == GDK_KEY_Return || ks == GDK_KEY_KP_Enter) {
		switcher_confirm();
		return TRUE;
	}
	if (ks == GDK_KEY_Tab) {
		switcher_select(sw_sel + 1);
		return TRUE;
	}
	/* Shift+Tab generates ISO_Left_Tab at the GDK level */
	if (ks == GDK_KEY_ISO_Left_Tab) {
		switcher_select(sw_sel - 1);
		return TRUE;
	}
	return FALSE;
}

static gboolean
on_key_release(GtkWidget *widget, GdkEventKey *ev, gpointer data)
{
	(void) widget;
	(void) data;

	guint ks = ev->keyval;

	/* Confirm on release of the modifier key that opened the switcher */
	if (ks == GDK_KEY_Alt_L || ks == GDK_KEY_Alt_R || ks == GDK_KEY_Super_L ||
	    ks == GDK_KEY_Super_R) {
		switcher_confirm();
		return TRUE;
	}
	return FALSE;
}

/* -------------------------------------------------------------------------
 * Show / hide / confirm / cancel
 * ---------------------------------------------------------------------- */

static void
switcher_hide(void)
{
	free_thumbnails();

	if (sw_entries) {
		free(sw_entries);
		sw_entries  = NULL;
		sw_nentries = 0;
	}

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
	switcher_hide();
	if (chosen)
		focus(chosen);
}

static void
switcher_cancel(void)
{
	switcher_hide();
}

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

void
switcher_init(void)
{
	if (sw_win)
		return; /* already initialised */

	sw_win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
	gtk_window_set_title(GTK_WINDOW(sw_win), "awm-switcher");
	gtk_window_set_decorated(GTK_WINDOW(sw_win), FALSE);
	gtk_window_set_skip_taskbar_hint(GTK_WINDOW(sw_win), TRUE);
	gtk_window_set_skip_pager_hint(GTK_WINDOW(sw_win), TRUE);
	gtk_window_set_keep_above(GTK_WINDOW(sw_win), TRUE);
	gtk_window_set_type_hint(GTK_WINDOW(sw_win), GDK_WINDOW_TYPE_HINT_DIALOG);

	/* Semi-transparent background via RGBA visual if available */
	{
		GdkScreen *gscreen = gtk_widget_get_screen(sw_win);
		GdkVisual *vis     = gdk_screen_get_rgba_visual(gscreen);
		if (vis)
			gtk_widget_set_visual(sw_win, vis);
		gtk_widget_set_app_paintable(sw_win, TRUE);
	}

	/* WM_CLASS so applyrules() can match it */
	gtk_window_set_wmclass(GTK_WINDOW(sw_win), "awm-switcher", "awm-switcher");

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

	/* Key events */
	gtk_widget_add_events(sw_win, GDK_KEY_PRESS_MASK | GDK_KEY_RELEASE_MASK);
	g_signal_connect(
	    sw_win, "key-press-event", G_CALLBACK(on_key_press), NULL);
	g_signal_connect(
	    sw_win, "key-release-event", G_CALLBACK(on_key_release), NULL);

	/* Intercept window close button (shouldn't appear, but be safe) */
	g_signal_connect(
	    sw_win, "delete-event", G_CALLBACK(gtk_widget_hide_on_delete), NULL);
}

void
switcher_show(const Arg *arg)
{
	int all_monitors = arg ? arg->i : 0;

	if (!sw_win)
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

	/* Walk the global client list (cl->clients is insertion order) */
	for (Client *c = cl->clients; c; c = c->next) {
		/* Skip hidden, SNI, and the switcher window itself */
		if (c->ishidden || c->issni)
			continue;
		/* Skip clients on other monitors when in single-monitor mode */
		if (!all_monitors && c->mon != selmon)
			continue;
		/* Client must be visible on its monitor's current tagset */
		if (!ISVISIBLE(c, c->mon))
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

	/* Start selection on the window after the currently focused one
	 * (classic Alt+Tab behaviour: second most recent = index 1). */
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
		total_w += SW_WIN_PAD; /* trailing gap */

		/* Cap to screen width */
		Monitor *m = selmon;
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
	gtk_window_present(GTK_WINDOW(sw_win));
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
switcher_cleanup(void)
{
}
int
switcher_active(void)
{
	return 0;
}

#endif /* COMPOSITOR */
