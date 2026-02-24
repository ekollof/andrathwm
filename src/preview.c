/* See LICENSE file for copyright and license details.
 *
 * preview.c — window preview popup for awm-ui
 *
 * Renders XComposite snapshot pixmaps as scaled thumbnails in a floating
 * GTK override-redirect window.  The layout is a horizontal row of cards,
 * each showing a thumbnail and the window title, mirroring the style used
 * by switcher.c in the WM process.
 *
 * Thumbnail rendering uses XRender server-side scaling on a dedicated XCB
 * connection opened independently of GDK.  Using GDK's own XCB connection
 * (via XGetXCBConnection) is unsafe: GDK owns the event queue and mixing raw
 * XCB requests on the same connection causes XCB to see unexpected
 * replies/errors and call exit() internally, silently killing awm-ui.
 * The dedicated connection shares the same X server as GDK (same DISPLAY)
 * so all pixmap XIDs obtained from awm are valid on it.
 *
 * Flow:
 *   1. preview_show() is called with the UiPreviewEntry array from SHM.
 *   2. For each entry with a non-zero pixmap_xid we build a scaled thumbnail
 *      via XRender and wrap it in a cairo_xcb_surface.
 *   3. GTK cards are built in a GtkScrolledWindow.
 *   4. Clicking a card sends UI_MSG_PREVIEW_FOCUS to awm and hides the popup.
 *   5. preview_hide() sends UI_MSG_PREVIEW_DONE with all snapshot XIDs so
 *      awm can call xcb_free_pixmap() on them.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

#include <xcb/xcb.h>
#include <xcb/render.h>
#include <xcb/xcb_renderutil.h>
#include <cairo/cairo.h>
#include <cairo/cairo-xcb.h>
#include <gtk/gtk.h>
#include <gdk/gdkx.h>

#include "log.h"
#include "preview.h"
#include "ui_proto.h"

/* -------------------------------------------------------------------------
 * Tunables
 * ---------------------------------------------------------------------- */

#define PV_MAX_THUMB_W 200
#define PV_MAX_THUMB_H 150
#define PV_MIN_CARD_W 120
#define PV_FALLBACK_W 120
#define PV_FALLBACK_H 80
#define PV_CARD_PAD 8
#define PV_CARD_GAP 6
#define PV_TITLE_H 28
#define PV_WIN_PAD 12
#define PV_BORDER_W 2

/* Scale a 96-DPI pixel constant by the current preview DPI */
#define PV_SCALE(px) ((int) ((px) * pv_dpi / 96.0 + 0.5))

/* -------------------------------------------------------------------------
 * Per-card state
 * ---------------------------------------------------------------------- */

typedef struct {
	uint32_t xwin;
	uint32_t pixmap_xid; /* snapshot XID (must be freed via DONE) */
	int      win_w;
	int      win_h;
	int      selected;
	int      depth; /* pixmap colour depth (24 or 32) */
	char     title[64];

	/* Thumbnail resources */
	xcb_pixmap_t         thumb_pixmap;
	xcb_render_picture_t thumb_pict;
	cairo_surface_t     *thumb_surf;
	int                  thumb_w;
	int                  thumb_h;
	int                  has_thumb;

	GtkWidget *card;
} PreviewCard;

/* -------------------------------------------------------------------------
 * Module state
 * ---------------------------------------------------------------------- */

static GtkWidget   *pv_win    = NULL;
static GtkWidget   *pv_scroll = NULL;
static GtkWidget   *pv_box    = NULL;
static PreviewCard *pv_cards  = NULL;
static unsigned int pv_ncard  = 0;
static int          pv_sel    = -1; /* selected card index */
static int          pv_ui_fd  = -1;
static double       pv_dpi    = 96.0; /* DPI from last UI_MSG_THEME */

/* XCB connection dedicated to XRender thumbnail work — opened independently
 * of GDK so that raw XCB requests do not interfere with GDK's event queue. */
static xcb_connection_t                      *pv_xc     = NULL;
static xcb_screen_t                          *pv_screen = NULL;
static xcb_render_query_pict_formats_reply_t *pv_rfmts  = NULL;

/* -------------------------------------------------------------------------
 * XCB / XRender helpers
 * ---------------------------------------------------------------------- */

/* Open a dedicated XCB connection for XRender work (call after gtk_init).
 * We use the same DISPLAY as GDK but open a separate connection so that
 * raw XCB calls do not race with GDK's event-queue ownership. */
static void
pv_ensure_xcb(void)
{
	const char           *dname;
	int                   scr_num = 0;
	xcb_screen_iterator_t it;
	int                   i;

	if (pv_xc)
		return;

	dname = gdk_display_get_name(gdk_display_get_default());
	pv_xc = xcb_connect(dname, &scr_num);
	if (!pv_xc || xcb_connection_has_error(pv_xc)) {
		awm_error("preview: xcb_connect failed for display %s",
		    dname ? dname : "(null)");
		pv_xc = NULL;
		return;
	}

	/* Walk to the screen at scr_num */
	it = xcb_setup_roots_iterator(xcb_get_setup(pv_xc));
	for (i = 0; i < scr_num; i++)
		xcb_screen_next(&it);
	pv_screen = it.data;

	/* Fetch render picture formats */
	xcb_render_query_pict_formats_cookie_t ck =
	    xcb_render_query_pict_formats(pv_xc);
	pv_rfmts = xcb_render_query_pict_formats_reply(pv_xc, ck, NULL);
}

static xcb_render_pictformat_t
pv_find_format_for_depth(int depth)
{
	const xcb_render_pictforminfo_t *fi;

	if (!pv_rfmts)
		return 0;

	if (depth == 32) {
		fi = xcb_render_util_find_standard_format(
		    pv_rfmts, XCB_PICT_STANDARD_ARGB_32);
		if (fi)
			return fi->id;
	}
	if (depth == 24) {
		fi = xcb_render_util_find_standard_format(
		    pv_rfmts, XCB_PICT_STANDARD_RGB_24);
		if (fi)
			return fi->id;
	}

	if (!pv_screen)
		return 0;
	const xcb_render_pictvisual_t *pv =
	    xcb_render_util_find_visual_format(pv_rfmts, pv_screen->root_visual);
	return pv ? pv->format : 0;
}

static xcb_visualtype_t *
pv_get_root_visual(void)
{
	if (!pv_screen)
		return NULL;
	xcb_depth_iterator_t dit = xcb_screen_allowed_depths_iterator(pv_screen);
	for (; dit.rem; xcb_depth_next(&dit)) {
		xcb_visualtype_iterator_t vit = xcb_depth_visuals_iterator(dit.data);
		for (; vit.rem; xcb_visualtype_next(&vit))
			if (vit.data->visual_id == pv_screen->root_visual)
				return vit.data;
	}
	return NULL;
}

/* Build scaled thumbnail resources for a card.
 * Leaves card->has_thumb = 0 on failure. */
static void
pv_build_thumb(PreviewCard *c)
{
	xcb_render_pictformat_t src_fmt, dst_fmt;
	xcb_render_picture_t    src_pict;
	xcb_render_transform_t  xform;
	double                  sx, sy, scale;
	int                     tw, th;
	uint32_t                pmask = 0;
	uint32_t                pval  = 0;
	uint8_t                 dst_depth;

	c->has_thumb    = 0;
	c->thumb_pixmap = 0;
	c->thumb_pict   = 0;
	c->thumb_surf   = NULL;

	if (!pv_xc || !pv_screen || !pv_rfmts)
		return;
	if (!c->pixmap_xid || c->win_w <= 0 || c->win_h <= 0)
		return;

	/* Compute thumbnail size preserving aspect ratio */
	sx    = (double) PV_SCALE(PV_MAX_THUMB_W) / (double) c->win_w;
	sy    = (double) PV_SCALE(PV_MAX_THUMB_H) / (double) c->win_h;
	scale = sx < sy ? sx : sy;
	if (scale > 1.0)
		scale = 1.0;
	tw = (int) (c->win_w * scale);
	th = (int) (c->win_h * scale);
	if (tw < 1)
		tw = 1;
	if (th < 1)
		th = 1;

	/* Source picture from snapshot pixmap — use actual pixmap depth */
	src_fmt = pv_find_format_for_depth(c->depth ? c->depth : 24);
	if (!src_fmt) {
		awm_warn("preview: no XRender format for depth %d", c->depth);
		return;
	}

	src_pict = xcb_generate_id(pv_xc);
	{
		xcb_void_cookie_t    ck;
		xcb_generic_error_t *err;
		ck  = xcb_render_create_picture_checked(pv_xc, src_pict,
		     (xcb_drawable_t) c->pixmap_xid, src_fmt, pmask, &pval);
		err = xcb_request_check(pv_xc, ck);
		if (err) {
			awm_warn("preview: create src picture failed (err %d, depth %d)",
			    (int) err->error_code, c->depth);
			free(err);
			src_pict = 0;
			return;
		}
	}

	/* Scale transform */
	{
		double             inv    = 1.0 / scale;
		xcb_render_fixed_t fp_inv = (xcb_render_fixed_t) (inv * 65536.0 + 0.5);
		xcb_render_fixed_t fp_one = 65536;
		xform.matrix11            = fp_inv;
		xform.matrix12            = 0;
		xform.matrix13            = 0;
		xform.matrix21            = 0;
		xform.matrix22            = fp_inv;
		xform.matrix23            = 0;
		xform.matrix31            = 0;
		xform.matrix32            = 0;
		xform.matrix33            = fp_one;
	}
	xcb_render_set_picture_transform(pv_xc, src_pict, xform);
	{
		static const char filter[] = "good";
		xcb_render_set_picture_filter(
		    pv_xc, src_pict, (uint16_t) (sizeof(filter) - 1), filter, 0, NULL);
	}

	/* Destination pixmap at thumbnail size */
	dst_depth = (uint8_t) pv_screen->root_depth;
	dst_fmt   = pv_find_format_for_depth(pv_screen->root_depth);
	if (!dst_fmt) {
		xcb_render_free_picture(pv_xc, src_pict);
		return;
	}

	c->thumb_pixmap = xcb_generate_id(pv_xc);
	{
		xcb_void_cookie_t    ck;
		xcb_generic_error_t *err;
		ck = xcb_create_pixmap_checked(pv_xc, dst_depth, c->thumb_pixmap,
		    (xcb_drawable_t) pv_screen->root, (uint16_t) tw, (uint16_t) th);
		xcb_flush(pv_xc);
		err = xcb_request_check(pv_xc, ck);
		if (err) {
			awm_warn("preview: create pixmap failed (err %d)",
			    (int) err->error_code);
			free(err);
			xcb_render_free_picture(pv_xc, src_pict);
			c->thumb_pixmap = 0;
			return;
		}
	}

	c->thumb_pict = xcb_generate_id(pv_xc);
	xcb_render_create_picture(pv_xc, c->thumb_pict,
	    (xcb_drawable_t) c->thumb_pixmap, dst_fmt, pmask, &pval);

	/* Pre-fill dst with opaque dark background so ARGB windows blend
	 * cleanly and do not show garbage from uninitialised pixmap memory. */
	{
		xcb_render_color_t bc;
		xcb_rectangle_t    br = { 0, 0, (uint16_t) tw, (uint16_t) th };
		bc.red                = 0x2000;
		bc.green              = 0x2000;
		bc.blue               = 0x2000;
		bc.alpha              = 0xffff;
		xcb_render_fill_rectangles(
		    pv_xc, XCB_RENDER_PICT_OP_SRC, c->thumb_pict, bc, 1, &br);
	}

	/* OVER blends pre-multiplied ARGB sources against the background;
	 * opaque depth-24 sources composite correctly with OVER as well. */
	xcb_render_composite(pv_xc, XCB_RENDER_PICT_OP_OVER, src_pict,
	    XCB_RENDER_PICTURE_NONE, c->thumb_pict, 0, 0, 0, 0, 0, 0,
	    (uint16_t) tw, (uint16_t) th);
	xcb_flush(pv_xc);
	xcb_render_free_picture(pv_xc, src_pict);

	/* Wrap in cairo surface */
	{
		xcb_visualtype_t *vis = pv_get_root_visual();
		if (!vis) {
			xcb_render_free_picture(pv_xc, c->thumb_pict);
			xcb_free_pixmap(pv_xc, c->thumb_pixmap);
			c->thumb_pict   = 0;
			c->thumb_pixmap = 0;
			return;
		}
		c->thumb_surf = cairo_xcb_surface_create(
		    pv_xc, (xcb_drawable_t) c->thumb_pixmap, vis, tw, th);
		if (!c->thumb_surf ||
		    cairo_surface_status(c->thumb_surf) != CAIRO_STATUS_SUCCESS) {
			if (c->thumb_surf)
				cairo_surface_destroy(c->thumb_surf);
			c->thumb_surf = NULL;
			xcb_render_free_picture(pv_xc, c->thumb_pict);
			xcb_free_pixmap(pv_xc, c->thumb_pixmap);
			c->thumb_pict   = 0;
			c->thumb_pixmap = 0;
			return;
		}
	}

	c->thumb_w   = tw;
	c->thumb_h   = th;
	c->has_thumb = 1;
}

/* Free thumbnail resources for one card.  Does NOT free the snapshot
 * pixmap_xid — that is returned to awm via PREVIEW_DONE. */
static void
pv_free_thumb(PreviewCard *c)
{
	if (c->thumb_surf) {
		cairo_surface_destroy(c->thumb_surf);
		c->thumb_surf = NULL;
	}
	if (c->thumb_pict && pv_xc) {
		xcb_render_free_picture(pv_xc, c->thumb_pict);
		c->thumb_pict = 0;
	}
	if (c->thumb_pixmap && pv_xc) {
		xcb_free_pixmap(pv_xc, c->thumb_pixmap);
		c->thumb_pixmap = 0;
	}
	c->has_thumb = 0;
}

/* -------------------------------------------------------------------------
 * GTK card rendering
 * ---------------------------------------------------------------------- */

static int
card_w(const PreviewCard *c)
{
	if (c->has_thumb) {
		int w = c->thumb_w + 2 * PV_SCALE(PV_CARD_PAD);
		return w < PV_SCALE(PV_MIN_CARD_W) ? PV_SCALE(PV_MIN_CARD_W) : w;
	}
	return PV_SCALE(PV_FALLBACK_W);
}

static int
card_h(const PreviewCard *c)
{
	if (c->has_thumb)
		return c->thumb_h + 2 * PV_SCALE(PV_CARD_PAD) + PV_SCALE(PV_TITLE_H);
	return PV_SCALE(PV_FALLBACK_H);
}

static gboolean
on_card_draw(GtkWidget *widget, cairo_t *cr, gpointer data)
{
	PreviewCard *c = (PreviewCard *) data;
	int          w = gtk_widget_get_allocated_width(widget);
	int          h = gtk_widget_get_allocated_height(widget);
	int sel = (c->selected && pv_sel < 0) || ((int) (c - pv_cards) == pv_sel);

	/* Background */
	if (sel)
		cairo_set_source_rgba(cr, 0.0, 0.33, 0.53, 0.92);
	else
		cairo_set_source_rgba(cr, 0.13, 0.13, 0.13, 0.88);
	cairo_rectangle(cr, 0, 0, w, h);
	cairo_fill(cr);

	/* Thumbnail */
	if (c->has_thumb && c->thumb_surf) {
		int tx = (w - c->thumb_w) / 2;
		int ty = PV_SCALE(PV_CARD_PAD);
		cairo_save(cr);
		cairo_set_source_surface(cr, c->thumb_surf, tx, ty);
		cairo_rectangle(cr, tx, ty, c->thumb_w, c->thumb_h);
		cairo_fill(cr);
		cairo_restore(cr);
	} else {
		/* Placeholder */
		int ph = h - PV_SCALE(PV_TITLE_H) - 2 * PV_SCALE(PV_CARD_PAD);
		if (ph > 0) {
			cairo_set_source_rgba(cr, 0.1, 0.1, 0.1, 0.5);
			cairo_rectangle(cr, PV_SCALE(PV_CARD_PAD), PV_SCALE(PV_CARD_PAD),
			    w - 2 * PV_SCALE(PV_CARD_PAD), ph);
			cairo_fill(cr);
		}
	}

	/* Title row */
	int title_y = h - PV_SCALE(PV_TITLE_H);
	{
		cairo_set_source_rgba(cr, 0.3, 0.3, 0.3, 0.6);
		cairo_set_line_width(cr, 1.0);
		cairo_move_to(cr, 0, title_y);
		cairo_line_to(cr, w, title_y);
		cairo_stroke(cr);
	}

	/* Title text */
	if (w > 2 * PV_SCALE(PV_CARD_PAD) && c->title[0]) {
		PangoLayout          *lay;
		PangoFontDescription *fdesc;
		int                   pw = 0, ph = 0;

		lay   = pango_cairo_create_layout(cr);
		fdesc = pango_font_description_from_string("Sans 9");
		pango_layout_set_font_description(lay, fdesc);
		pango_font_description_free(fdesc);
		pango_layout_set_width(
		    lay, (w - 2 * PV_SCALE(PV_CARD_PAD)) * PANGO_SCALE);
		pango_layout_set_ellipsize(lay, PANGO_ELLIPSIZE_END);
		pango_layout_set_single_paragraph_mode(lay, TRUE);
		pango_layout_set_text(lay, c->title, -1);
		pango_layout_get_pixel_size(lay, &pw, &ph);

		cairo_set_source_rgba(cr, 0.88, 0.88, 0.88, 1.0);
		cairo_move_to(cr, PV_SCALE(PV_CARD_PAD),
		    title_y + (PV_SCALE(PV_TITLE_H) - ph) / 2);
		pango_cairo_show_layout(cr, lay);
		g_object_unref(lay);
		(void) pw;
	}

	/* Selection border */
	if (sel) {
		int bw = PV_SCALE(PV_BORDER_W);
		cairo_set_source_rgba(cr, 0.2, 0.6, 0.9, 1.0);
		cairo_set_line_width(cr, (double) bw);
		cairo_rectangle(cr, bw / 2.0, bw / 2.0, w - bw, h - bw);
		cairo_stroke(cr);
	}

	return FALSE;
}

/* Click on a card: send PREVIEW_FOCUS and hide */
static gboolean
on_card_click(GtkWidget *widget, GdkEventButton *ev, gpointer data)
{
	PreviewCard          *c = (PreviewCard *) data;
	UiPreviewFocusPayload fp;
	uint8_t      buf[sizeof(UiMsgHeader) + sizeof(UiPreviewFocusPayload)];
	UiMsgHeader *hdr = (UiMsgHeader *) buf;
	(void) widget;
	(void) ev;

	fp.xwin          = c->xwin;
	hdr->type        = (uint32_t) UI_MSG_PREVIEW_FOCUS;
	hdr->payload_len = sizeof(fp);
	memcpy(buf + sizeof(UiMsgHeader), &fp, sizeof(fp));

	if (pv_ui_fd >= 0)
		send(pv_ui_fd, buf, sizeof(buf), MSG_NOSIGNAL);

	preview_hide();
	return TRUE;
}

/* -------------------------------------------------------------------------
 * Send PREVIEW_DONE — return snapshot XIDs to awm
 * ---------------------------------------------------------------------- */

static void
pv_send_done(void)
{
	UiPreviewDonePayload dp;
	uint8_t              buf[sizeof(UiMsgHeader) + sizeof(dp)];
	UiMsgHeader         *hdr = (UiMsgHeader *) buf;
	unsigned int         i;

	if (pv_ui_fd < 0)
		return;

	memset(&dp, 0, sizeof(dp));
	dp.count = 0;
	for (i = 0; i < pv_ncard; i++) {
		if (pv_cards[i].pixmap_xid && dp.count < 32) {
			dp.xids[dp.count++]    = pv_cards[i].pixmap_xid;
			pv_cards[i].pixmap_xid = 0;
		}
	}

	hdr->type        = (uint32_t) UI_MSG_PREVIEW_DONE;
	hdr->payload_len = sizeof(dp);
	memcpy(buf + sizeof(UiMsgHeader), &dp, sizeof(dp));
	send(pv_ui_fd, buf, sizeof(buf), MSG_NOSIGNAL);
}

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

int
preview_init(int ui_send_fd)
{
	pv_ui_fd = ui_send_fd;
	pv_ensure_xcb();
	return 0;
}

void
preview_update_theme(const UiThemePayload *t)
{
	if (t && t->dpi > 0.0)
		pv_dpi = t->dpi;
}

void
preview_show(const UiPreviewEntry *entries, unsigned int count, int anchor_x,
    int anchor_y)
{
	unsigned int i;

	if (count == 0)
		return;

	/* Hide any currently visible preview first */
	if (pv_win)
		preview_hide();

	pv_ensure_xcb();

	/* Allocate card array */
	pv_cards = (PreviewCard *) calloc(count, sizeof(PreviewCard));
	if (!pv_cards)
		return;
	pv_ncard = count;
	pv_sel   = -1;

	for (i = 0; i < count; i++) {
		PreviewCard *c = &pv_cards[i];
		c->xwin        = entries[i].xwin;
		c->pixmap_xid  = entries[i].pixmap_xid;
		c->win_w       = (int) entries[i].w;
		c->win_h       = (int) entries[i].h;
		c->selected    = (int) entries[i].selected;
		c->depth       = (int) entries[i].depth;
		memcpy(c->title, entries[i].title, sizeof(c->title));
		c->title[sizeof(c->title) - 1] = '\0';
		pv_build_thumb(c);
	}

	/* Create GTK window */
	pv_win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
	gtk_window_set_decorated(GTK_WINDOW(pv_win), FALSE);
	gtk_window_set_skip_taskbar_hint(GTK_WINDOW(pv_win), TRUE);
	gtk_window_set_skip_pager_hint(GTK_WINDOW(pv_win), TRUE);
	gtk_window_set_type_hint(
	    GTK_WINDOW(pv_win), GDK_WINDOW_TYPE_HINT_POPUP_MENU);

	{
		GdkScreen *gs  = gtk_widget_get_screen(pv_win);
		GdkVisual *vis = gdk_screen_get_rgba_visual(gs);
		if (vis)
			gtk_widget_set_visual(pv_win, vis);
		gtk_widget_set_app_paintable(pv_win, TRUE);
	}

	{
		gtk_widget_realize(pv_win);
		GdkWindow *gwin = gtk_widget_get_window(pv_win);
		if (gwin)
			gdk_window_set_override_redirect(gwin, TRUE);
	}

	GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_container_set_border_width(
	    GTK_CONTAINER(outer), (guint) PV_SCALE(PV_WIN_PAD));
	gtk_container_add(GTK_CONTAINER(pv_win), outer);

	pv_scroll = gtk_scrolled_window_new(NULL, NULL);
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(pv_scroll),
	    GTK_POLICY_AUTOMATIC, GTK_POLICY_NEVER);
	gtk_box_pack_start(GTK_BOX(outer), pv_scroll, FALSE, FALSE, 0);

	pv_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	gtk_container_add(GTK_CONTAINER(pv_scroll), pv_box);

	/* Build cards */
	int total_w = 2 * PV_SCALE(PV_WIN_PAD);
	int max_h   = PV_SCALE(PV_FALLBACK_H);

	for (i = 0; i < pv_ncard; i++) {
		PreviewCard *c  = &pv_cards[i];
		int          cw = card_w(c);
		int          ch = card_h(c);

		GtkWidget *da = gtk_drawing_area_new();
		gtk_widget_set_size_request(da, cw, ch);
		g_signal_connect(da, "draw", G_CALLBACK(on_card_draw), c);
		gtk_widget_add_events(da, GDK_BUTTON_PRESS_MASK);
		g_signal_connect(
		    da, "button-press-event", G_CALLBACK(on_card_click), c);
		gtk_box_pack_start(GTK_BOX(pv_box), da, FALSE, FALSE,
		    (guint) (PV_SCALE(PV_CARD_GAP) / 2));
		c->card = da;

		total_w += cw + PV_SCALE(PV_CARD_GAP);
		if (ch > max_h)
			max_h = ch;
	}
	total_w += PV_SCALE(PV_WIN_PAD);

	/* Position: centre above anchor point, but ensure it stays on screen */
	{
		int win_h = max_h + 2 * PV_SCALE(PV_WIN_PAD);
		int win_x = anchor_x - total_w / 2;
		int win_y = anchor_y - win_h - 4; /* 4px gap above bar */

		/* Clamp to visible area — use GDK default screen geometry */
		GdkScreen *gs = gdk_screen_get_default();
		int        sw = gdk_screen_get_width(gs);
		int        sh = gdk_screen_get_height(gs);
		if (win_x + total_w > sw)
			win_x = sw - total_w;
		if (win_x < 0)
			win_x = 0;
		if (win_y < 0)
			win_y = anchor_y + 4; /* flip below bar */
		if (win_y + win_h > sh)
			win_y = sh - win_h;

		gtk_window_resize(GTK_WINDOW(pv_win), total_w, win_h);
		gtk_window_move(GTK_WINDOW(pv_win), win_x, win_y);
	}

	g_signal_connect(
	    pv_win, "delete-event", G_CALLBACK(gtk_widget_hide_on_delete), NULL);

	gtk_widget_show_all(pv_win);

	while (gtk_events_pending())
		gtk_main_iteration_do(FALSE);
}

void
preview_hide(void)
{
	unsigned int i;

	if (!pv_win && !pv_cards)
		return;

	/* Return snapshot pixmaps to awm */
	pv_send_done();

	/* Free thumbnail resources */
	for (i = 0; i < pv_ncard; i++)
		pv_free_thumb(&pv_cards[i]);
	free(pv_cards);
	pv_cards = NULL;
	pv_ncard = 0;
	pv_sel   = -1;

	if (pv_win) {
		gtk_widget_destroy(pv_win);
		pv_win    = NULL;
		pv_scroll = NULL;
		pv_box    = NULL;
	}
}

void
preview_cleanup(void)
{
	preview_hide();
	if (pv_rfmts) {
		free(pv_rfmts);
		pv_rfmts = NULL;
	}
	if (pv_xc) {
		xcb_disconnect(pv_xc);
		pv_xc = NULL;
	}
	pv_screen = NULL;
}
