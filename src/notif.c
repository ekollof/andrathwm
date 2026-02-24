/* See LICENSE file for copyright and license details.
 *
 * notif.c — org.freedesktop.Notifications daemon for awm-ui
 *
 * Implements the freedesktop Desktop Notifications specification v1.2.
 *
 * Supported:
 *   - Notify (creates/replaces popup, schedules auto-expire)
 *   - CloseNotification
 *   - GetCapabilities: body, body-markup, icon-static, actions, urgency
 *   - GetServerInformation
 *   - Signals: NotificationClosed, ActionInvoked
 *
 * Popup placement: configurable anchor (TopRight / BottomRight / TopLeft /
 * BottomLeft) via NOTIF_ANCHOR constant; each popup stacks inward from the
 * anchor corner with NOTIF_GAP pixels between them.
 *
 * Urgency levels (from hints["urgency"] byte):
 *   0 = Low   — grey stripe
 *   1 = Normal (default) — blue stripe
 *   2 = Critical — red stripe, never auto-expires
 *
 * Inline icon (hints["image-data"] a(iiibiiay)):
 *   Width, Height, RowStride, HasAlpha, BitsPerSample, Channels, Data.
 *   Converted to a cairo surface via a manual pixel copy.
 *
 * config.h integration:
 *   NOTIF_ANCHOR       one of: TopRight, BottomRight, TopLeft, BottomLeft
 *   NOTIF_WIDTH        popup width in pixels
 *   NOTIF_DEFAULT_TIMEOUT  ms; used when dbus timeout == -1 or 0
 *   NOTIF_MAX_VISIBLE  maximum simultaneous popups
 *   NOTIF_MARGIN_X     horizontal distance from screen edge
 *   NOTIF_MARGIN_Y     vertical distance from screen edge
 *   NOTIF_GAP          gap between stacked popups
 */

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <cairo/cairo.h>
#include <dbus/dbus.h>
#include <gtk/gtk.h>
#include <pango/pangocairo.h>

#include "icon.h"
#include "log.h"
#include "notif.h"

/* -------------------------------------------------------------------------
 * Markup helpers
 * ---------------------------------------------------------------------- */

/*
 * Set layout text from a string that may contain Pango markup (as sent by
 * notification senders that advertise body-markup support).  If the string
 * is not valid Pango markup (e.g. unescaped '&' or bare '<'), fall back to
 * treating it as plain text so the raw string is shown rather than nothing.
 */
static void
layout_set_markup_safe(PangoLayout *lay, const char *text)
{
	GError *err = NULL;

	if (!text || !text[0]) {
		pango_layout_set_text(lay, "", -1);
		return;
	}

	if (pango_parse_markup(text, -1, 0, NULL, NULL, NULL, &err)) {
		pango_layout_set_markup(lay, text, -1);
	} else {
		/* Not valid markup — render as plain text */
		g_error_free(err);
		pango_layout_set_text(lay, text, -1);
	}
}

/* -------------------------------------------------------------------------
 * Config defaults — overridden by constants from config.h when available.
 * ---------------------------------------------------------------------- */

#ifndef NOTIF_ANCHOR
#define NOTIF_ANCHOR TopRight
#endif
#ifndef NOTIF_WIDTH
#define NOTIF_WIDTH 320
#endif
#ifndef NOTIF_DEFAULT_TIMEOUT
#define NOTIF_DEFAULT_TIMEOUT 5000
#endif
#ifndef NOTIF_MAX_VISIBLE
#define NOTIF_MAX_VISIBLE 5
#endif
#ifndef NOTIF_MARGIN_X
#define NOTIF_MARGIN_X 12
#endif
#ifndef NOTIF_MARGIN_Y
#define NOTIF_MARGIN_Y 12
#endif
#ifndef NOTIF_GAP
#define NOTIF_GAP 6
#endif

/* Anchor enum — values match the macro names so config.h can use them */
typedef enum {
	TopRight    = 0,
	BottomRight = 1,
	TopLeft     = 2,
	BottomLeft  = 3,
} NotifAnchor;

/* -------------------------------------------------------------------------
 * D-Bus interface / path constants
 * ---------------------------------------------------------------------- */

#define NOTIF_BUS_NAME "org.freedesktop.Notifications"
#define NOTIF_OBJECT_PATH "/org/freedesktop/Notifications"
#define NOTIF_INTERFACE "org.freedesktop.Notifications"

/* -------------------------------------------------------------------------
 * Internal notification record
 * ---------------------------------------------------------------------- */

typedef struct NotifItem NotifItem;
struct NotifItem {
	uint32_t         id;
	char            *app_name;
	char            *summary;
	char            *body;
	char            *icon_name;  /* may be NULL */
	cairo_surface_t *icon_surf;  /* inline image-data surface, or NULL */
	int              urgency;    /* 0=low 1=normal 2=critical */
	int              timeout_ms; /* -1 = never */
	gint64           expire_at;  /* g_get_monotonic_time() target, 0=never */
	GtkWidget       *win;        /* toplevel GTK window */
	GtkWidget       *da;         /* GtkDrawingArea inside window */
	guint            timer_id;   /* GLib timeout source id */
	int              h;          /* current popup height in pixels */
	NotifItem       *next;
};

/* -------------------------------------------------------------------------
 * Module state
 * ---------------------------------------------------------------------- */

static DBusConnection *notif_conn  = NULL;
static uint32_t        notif_seq   = 1;    /* next notification id */
static NotifItem      *notif_list  = NULL; /* head of visible list */
static int             notif_count = 0;

/* Monitor workarea — updated by notif_update_geom() */
static int notif_mon_wx   = 0;
static int notif_mon_wy   = 0;
static int notif_mon_ww   = 0;
static int notif_mon_wh   = 0;
static int notif_geom_set = 0; /* 0 until first UI_MSG_MONITOR_GEOM */

/* Visual theme — updated by notif_update_theme() */
static UiThemePayload notif_theme;
static int            notif_theme_set = 0;

/* Forward declarations */
static void notif_item_close(NotifItem *it, uint32_t reason);
static void notif_restack(void);

/* -------------------------------------------------------------------------
 * Popup geometry helpers
 * ---------------------------------------------------------------------- */

/* Compute the desired popup height given its content.
 *
 * We measure text via a 1×1 Cairo image surface so that the Pango context
 * picks up the same DPI/resolution as the real drawing context in
 * on_popup_draw().  Using pango_font_map_create_context() directly produces
 * a context with no surface attached and therefore a hard-coded 96 DPI,
 * which causes height underestimates on HiDPI screens. */
static int
popup_height(NotifItem *it)
{
	cairo_surface_t      *probe_surf;
	cairo_t              *probe_cr;
	PangoLayout          *lay;
	PangoFontDescription *fdesc;
	int                   text_h = 0;
	int                   bh     = 0;

	/* 1×1 surface is enough — we only need a context with correct DPI. */
	probe_surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
	probe_cr   = cairo_create(probe_surf);
	lay        = pango_cairo_create_layout(probe_cr);

	/* Summary line */
	if (notif_theme_set && notif_theme.font[0]) {
		fdesc = pango_font_description_from_string(notif_theme.font);
		pango_layout_set_font_description(lay, fdesc);
		pango_font_description_free(fdesc);
	}
	pango_layout_set_width(
	    lay, (NOTIF_WIDTH - 56) * PANGO_SCALE); /* 56 = icon(32)+pad(12)*2 */
	layout_set_markup_safe(lay, it->summary ? it->summary : "");
	{
		int pw = 0, ph = 0;
		pango_layout_get_pixel_size(lay, &pw, &ph);
		text_h += ph + 4;
		(void) pw;
	}

	/* Body */
	if (it->body && it->body[0]) {
		if (notif_theme_set && notif_theme.font[0]) {
			fdesc = pango_font_description_from_string(notif_theme.font);
			pango_layout_set_font_description(lay, fdesc);
			pango_font_description_free(fdesc);
		}
		pango_layout_set_width(lay, (NOTIF_WIDTH - 56) * PANGO_SCALE);
		layout_set_markup_safe(lay, it->body);
		pango_layout_set_wrap(lay, PANGO_WRAP_WORD_CHAR);
		{
			int pw = 0, ph = 0;
			pango_layout_get_pixel_size(lay, &pw, &ph);
			bh = ph;
			(void) pw;
		}
	}

	g_object_unref(lay);
	cairo_destroy(probe_cr);
	cairo_surface_destroy(probe_surf);

	{
		int h = 12 + text_h + bh + 12; /* top-pad + summary + body + bot-pad */
		if (h < 56)
			h = 56; /* minimum: icon height (32) + 2*pad(12) */
		return h;
	}
}

/* Compute the top-left screen position for popup slot idx (0 = closest to
 * the anchor corner). */
static void
popup_position(int idx, int pop_h, int *out_x, int *out_y)
{
	NotifAnchor anchor = (NotifAnchor) NOTIF_ANCHOR;
	int         x, y;

	/* Accumulate height of all previous popups in the stack */
	int stack_off = 0;
	{
		int        slot = 0;
		NotifItem *it   = notif_list;
		while (it && slot < idx) {
			stack_off += it->h + NOTIF_GAP;
			slot++;
			it = it->next;
		}
	}

	switch (anchor) {
	case TopRight:
	default:
		x = notif_mon_wx + notif_mon_ww - NOTIF_WIDTH - NOTIF_MARGIN_X;
		y = notif_mon_wy + NOTIF_MARGIN_Y + stack_off;
		break;
	case BottomRight:
		x = notif_mon_wx + notif_mon_ww - NOTIF_WIDTH - NOTIF_MARGIN_X;
		y = notif_mon_wy + notif_mon_wh - NOTIF_MARGIN_Y - pop_h - stack_off;
		break;
	case TopLeft:
		x = notif_mon_wx + NOTIF_MARGIN_X;
		y = notif_mon_wy + NOTIF_MARGIN_Y + stack_off;
		break;
	case BottomLeft:
		x = notif_mon_wx + NOTIF_MARGIN_X;
		y = notif_mon_wy + notif_mon_wh - NOTIF_MARGIN_Y - pop_h - stack_off;
		break;
	}

	*out_x = x;
	*out_y = y;
}

/* -------------------------------------------------------------------------
 * GTK popup rendering
 * ---------------------------------------------------------------------- */

static gboolean
on_popup_draw(GtkWidget *widget, cairo_t *cr, gpointer data)
{
	NotifItem *it = (NotifItem *) data;
	int        w  = gtk_widget_get_allocated_width(widget);
	int        h  = gtk_widget_get_allocated_height(widget);

	/* Background — use theme norm_bg if available, else dark fallback */
	if (notif_theme_set) {
		cairo_set_source_rgba(cr, notif_theme.norm_bg[0] / 65535.0,
		    notif_theme.norm_bg[1] / 65535.0, notif_theme.norm_bg[2] / 65535.0,
		    0.93);
	} else {
		cairo_set_source_rgba(cr, 0.13, 0.13, 0.13, 0.93);
	}
	cairo_rectangle(cr, 0, 0, w, h);
	cairo_fill(cr);

	/* Urgency stripe on the left */
	{
		double r, g, b;
		if (it->urgency == 2) {
			r = 0.85;
			g = 0.15;
			b = 0.15; /* critical — red */
		} else if (it->urgency == 0) {
			r = 0.5;
			g = 0.5;
			b = 0.5; /* low — grey */
		} else {
			r = 0.15;
			g = 0.45;
			b = 0.85; /* normal — blue */
		}
		cairo_set_source_rgb(cr, r, g, b);
		cairo_rectangle(cr, 0, 0, 4, h);
		cairo_fill(cr);
	}

	/* Icon — inline image-data surface or theme icon */
	cairo_surface_t *icon_surf = it->icon_surf;
	if (!icon_surf && it->icon_name && it->icon_name[0])
		icon_surf = icon_load(it->icon_name, 32);

	if (icon_surf) {
		int    isw    = cairo_image_surface_get_width(icon_surf);
		int    ish    = cairo_image_surface_get_height(icon_surf);
		double iscale = (isw > 0 && ish > 0)
		    ? (32.0 / (isw > ish ? (double) isw : (double) ish))
		    : 1.0;
		int    ix     = 12 + 4; /* after stripe */
		int    iy     = (h - 32) / 2;
		cairo_save(cr);
		cairo_translate(cr, ix, iy);
		cairo_scale(cr, iscale, iscale);
		cairo_set_source_surface(cr, icon_surf, 0, 0);
		cairo_paint(cr);
		cairo_restore(cr);
		/* Only destroy if we loaded it locally in this call */
		if (icon_surf != it->icon_surf)
			cairo_surface_destroy(icon_surf);
	}

	/* Text */
	{
		int tx = 12 + 4 + 32 + 8; /* stripe + padding + icon + gap */
		int ty = 12;
		int tw = w - tx - 8;

		PangoLayout *lay = pango_cairo_create_layout(cr);

		/* Summary — bold, use sel_fg if theme available */
		if (tw > 0 && it->summary) {
			const char *font_str = (notif_theme_set && notif_theme.font[0])
			    ? notif_theme.font
			    : "Sans Bold 10";
			PangoFontDescription *fdesc =
			    pango_font_description_from_string(font_str);
			pango_layout_set_font_description(lay, fdesc);
			pango_font_description_free(fdesc);
			pango_layout_set_width(lay, tw * PANGO_SCALE);
			pango_layout_set_ellipsize(lay, PANGO_ELLIPSIZE_END);
			pango_layout_set_single_paragraph_mode(lay, TRUE);
			layout_set_markup_safe(lay, it->summary);
			if (notif_theme_set) {
				cairo_set_source_rgba(cr, notif_theme.sel_fg[0] / 65535.0,
				    notif_theme.sel_fg[1] / 65535.0,
				    notif_theme.sel_fg[2] / 65535.0, 1.0);
			} else {
				cairo_set_source_rgba(cr, 0.92, 0.92, 0.92, 1.0);
			}
			cairo_move_to(cr, tx, ty);
			pango_cairo_show_layout(cr, lay);

			int pw = 0, ph = 0;
			pango_layout_get_pixel_size(lay, &pw, &ph);
			ty += ph + 4;
			(void) pw;
		}

		/* Body — use norm_fg if theme available */
		if (tw > 0 && it->body && it->body[0]) {
			const char *font_str = (notif_theme_set && notif_theme.font[0])
			    ? notif_theme.font
			    : "Sans 9";
			PangoFontDescription *fdesc =
			    pango_font_description_from_string(font_str);
			pango_layout_set_font_description(lay, fdesc);
			pango_font_description_free(fdesc);
			pango_layout_set_width(lay, tw * PANGO_SCALE);
			pango_layout_set_ellipsize(lay, PANGO_ELLIPSIZE_NONE);
			pango_layout_set_single_paragraph_mode(lay, FALSE);
			pango_layout_set_wrap(lay, PANGO_WRAP_WORD_CHAR);
			layout_set_markup_safe(lay, it->body);
			if (notif_theme_set) {
				cairo_set_source_rgba(cr, notif_theme.norm_fg[0] / 65535.0,
				    notif_theme.norm_fg[1] / 65535.0,
				    notif_theme.norm_fg[2] / 65535.0, 1.0);
			} else {
				cairo_set_source_rgba(cr, 0.7, 0.7, 0.7, 1.0);
			}
			cairo_move_to(cr, tx, ty);
			pango_cairo_show_layout(cr, lay);
		}

		g_object_unref(lay);
	}

	/* Border — use theme norm_bd if available */
	if (notif_theme_set) {
		cairo_set_source_rgba(cr, notif_theme.norm_bd[0] / 65535.0,
		    notif_theme.norm_bd[1] / 65535.0, notif_theme.norm_bd[2] / 65535.0,
		    0.8);
	} else {
		cairo_set_source_rgba(cr, 0.3, 0.3, 0.3, 0.8);
	}
	cairo_set_line_width(cr, 1.0);
	cairo_rectangle(cr, 0.5, 0.5, w - 1, h - 1);
	cairo_stroke(cr);

	return FALSE;
}

/* Click on popup: close it (reason 2 = dismissed by user) */
static gboolean
on_popup_button_press(GtkWidget *widget, GdkEventButton *ev, gpointer data)
{
	(void) widget;
	(void) ev;
	notif_item_close((NotifItem *) data, 2);
	return TRUE;
}

/* -------------------------------------------------------------------------
 * Popup lifetime
 * ---------------------------------------------------------------------- */

/* Auto-expire timer callback. */
static gboolean
notif_expire_cb(gpointer data)
{
	NotifItem *it = (NotifItem *) data;
	it->timer_id  = 0;
	notif_item_close(it, 1); /* reason 1 = expired */
	return G_SOURCE_REMOVE;
}

/* Create and show the GTK popup window for a notification item.
 * Does not position it — call notif_restack() afterwards. */
static void
notif_item_show(NotifItem *it)
{
	/* Defer until we have real monitor geometry from awm */
	if (!notif_geom_set)
		return;

	it->h = popup_height(it);

	it->win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
	gtk_window_set_decorated(GTK_WINDOW(it->win), FALSE);
	gtk_window_set_skip_taskbar_hint(GTK_WINDOW(it->win), TRUE);
	gtk_window_set_skip_pager_hint(GTK_WINDOW(it->win), TRUE);
	gtk_window_set_type_hint(
	    GTK_WINDOW(it->win), GDK_WINDOW_TYPE_HINT_NOTIFICATION);

	/* RGBA visual for semi-transparency */
	{
		GdkScreen *gscreen = gtk_widget_get_screen(it->win);
		GdkVisual *vis     = gdk_screen_get_rgba_visual(gscreen);
		if (vis)
			gtk_widget_set_visual(it->win, vis);
		gtk_widget_set_app_paintable(it->win, TRUE);
	}

	/* Override-redirect: the WM will not manage this popup */
	{
		gtk_widget_realize(it->win);
		GdkWindow *gwin = gtk_widget_get_window(it->win);
		if (gwin)
			gdk_window_set_override_redirect(gwin, TRUE);
	}

	gtk_window_resize(GTK_WINDOW(it->win), NOTIF_WIDTH, it->h);

	it->da = gtk_drawing_area_new();
	gtk_widget_set_size_request(it->da, NOTIF_WIDTH, it->h);
	gtk_container_add(GTK_CONTAINER(it->win), it->da);

	g_signal_connect(it->da, "draw", G_CALLBACK(on_popup_draw), it);
	gtk_widget_add_events(
	    it->win, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK);
	g_signal_connect(
	    it->win, "button-press-event", G_CALLBACK(on_popup_button_press), it);

	gtk_widget_show_all(it->win);

	/* Schedule auto-expire */
	if (it->timeout_ms > 0 && it->urgency != 2) {
		it->timer_id =
		    g_timeout_add((guint) it->timeout_ms, notif_expire_cb, it);
	}
}

/* Emit NotificationClosed signal on D-Bus. */
static void
notif_emit_closed(uint32_t id, uint32_t reason)
{
	DBusMessage *sig;

	if (!notif_conn)
		return;

	sig = dbus_message_new_signal(
	    NOTIF_OBJECT_PATH, NOTIF_INTERFACE, "NotificationClosed");
	if (!sig)
		return;

	dbus_message_append_args(sig, DBUS_TYPE_UINT32, &id, DBUS_TYPE_UINT32,
	    &reason, DBUS_TYPE_INVALID);
	dbus_connection_send(notif_conn, sig, NULL);
	dbus_message_unref(sig);
	dbus_connection_flush(notif_conn);
}

/* Remove a notification item from the list and destroy its window. */
static void
notif_item_close(NotifItem *it, uint32_t reason)
{
	NotifItem **pp;
	uint32_t    id = it->id;

	/* Cancel timer */
	if (it->timer_id) {
		g_source_remove(it->timer_id);
		it->timer_id = 0;
	}

	/* Destroy window */
	if (it->win) {
		gtk_widget_destroy(it->win);
		it->win = NULL;
		it->da  = NULL;
	}

	/* Remove from list */
	for (pp = &notif_list; *pp; pp = &(*pp)->next) {
		if (*pp == it) {
			*pp = it->next;
			notif_count--;
			break;
		}
	}

	/* Free resources */
	free(it->app_name);
	free(it->summary);
	free(it->body);
	free(it->icon_name);
	if (it->icon_surf) {
		cairo_surface_destroy(it->icon_surf);
		it->icon_surf = NULL;
	}
	free(it);

	notif_emit_closed(id, reason);
	notif_restack();
}

/* Recompute and update on-screen positions of all visible popups. */
static void
notif_restack(void)
{
	int        idx = 0;
	NotifItem *it;

	for (it = notif_list; it; it = it->next, idx++) {
		if (!it->win)
			continue;
		int x, y;
		popup_position(idx, it->h, &x, &y);
		gtk_window_move(GTK_WINDOW(it->win), x, y);
	}
}

/* Build a cairo surface from inline image-data (a(iiibiiay)) hint.
 * Returns a new surface on success, NULL on failure. */
static cairo_surface_t *
notif_image_data_to_surface(DBusMessageIter *iter_v)
{
	/* Unwrap variant → struct (iiibiiay) */
	DBusMessageIter iter_s, iter_a;
	int             w, h, rowstride;
	int             has_alpha, bps, channels;

	if (dbus_message_iter_get_arg_type(iter_v) != DBUS_TYPE_STRUCT)
		return NULL;

	dbus_message_iter_recurse(iter_v, &iter_s);

#define ITER_GET_INT(dst)                                               \
	do {                                                                \
		if (dbus_message_iter_get_arg_type(&iter_s) != DBUS_TYPE_INT32) \
			return NULL;                                                \
		dbus_message_iter_get_basic(&iter_s, &(dst));                   \
		dbus_message_iter_next(&iter_s);                                \
	} while (0)

	ITER_GET_INT(w);
	ITER_GET_INT(h);
	ITER_GET_INT(rowstride);
	{
		int ba = 0;
		if (dbus_message_iter_get_arg_type(&iter_s) != DBUS_TYPE_BOOLEAN)
			return NULL;
		dbus_message_iter_get_basic(&iter_s, &ba);
		has_alpha = ba;
		dbus_message_iter_next(&iter_s);
	}
	ITER_GET_INT(bps);
	ITER_GET_INT(channels);

#undef ITER_GET_INT

	/* Array of bytes */
	if (dbus_message_iter_get_arg_type(&iter_s) != DBUS_TYPE_ARRAY)
		return NULL;
	dbus_message_iter_recurse(&iter_s, &iter_a);

	const uint8_t *src_data = NULL;
	int            src_len  = 0;
	dbus_message_iter_get_fixed_array(
	    &iter_a, (const void **) &src_data, &src_len);

	if (!src_data || src_len <= 0 || w <= 0 || h <= 0 || bps != 8)
		return NULL;

	cairo_surface_t *surf =
	    cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
	if (cairo_surface_status(surf) != CAIRO_STATUS_SUCCESS) {
		cairo_surface_destroy(surf);
		return NULL;
	}

	uint32_t *dst     = (uint32_t *) cairo_image_surface_get_data(surf);
	int       dstride = cairo_image_surface_get_stride(surf) / 4;

	cairo_surface_flush(surf);
	for (int row = 0; row < h; row++) {
		const uint8_t *srow = src_data + row * rowstride;
		uint32_t      *drow = dst + row * dstride;
		for (int col = 0; col < w; col++) {
			uint8_t r, g, b, a;
			if (channels >= 4 && has_alpha) {
				r = srow[col * 4 + 0];
				g = srow[col * 4 + 1];
				b = srow[col * 4 + 2];
				a = srow[col * 4 + 3];
			} else {
				r = srow[col * 3 + 0];
				g = srow[col * 3 + 1];
				b = srow[col * 3 + 2];
				a = 0xff;
			}
			/* Cairo ARGB32 is premultiplied */
			drow[col] = ((uint32_t) a << 24) |
			    ((uint32_t) ((r * a + 127) / 255) << 16) |
			    ((uint32_t) ((g * a + 127) / 255) << 8) |
			    ((uint32_t) ((b * a + 127) / 255));
		}
	}
	cairo_surface_mark_dirty(surf);

	return surf;
}

/* -------------------------------------------------------------------------
 * D-Bus method handlers
 * ---------------------------------------------------------------------- */

static DBusHandlerResult
handle_notify(DBusConnection *conn, DBusMessage *msg)
{
	DBusMessageIter iter;
	const char     *app_name    = "";
	uint32_t        replaces_id = 0;
	const char     *app_icon    = "";
	const char     *summary     = "";
	const char     *body        = "";
	/* actions array — ignored for now */
	int              timeout_ms;
	int              urgency  = 1;
	cairo_surface_t *img_surf = NULL;

	dbus_message_iter_init(msg, &iter);

	/* app_name */
	if (dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_STRING) {
		dbus_message_iter_get_basic(&iter, &app_name);
		dbus_message_iter_next(&iter);
	}
	/* replaces_id */
	if (dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_UINT32) {
		dbus_message_iter_get_basic(&iter, &replaces_id);
		dbus_message_iter_next(&iter);
	}
	/* app_icon */
	if (dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_STRING) {
		dbus_message_iter_get_basic(&iter, &app_icon);
		dbus_message_iter_next(&iter);
	}
	/* summary */
	if (dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_STRING) {
		dbus_message_iter_get_basic(&iter, &summary);
		dbus_message_iter_next(&iter);
	}
	/* body */
	if (dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_STRING) {
		dbus_message_iter_get_basic(&iter, &body);
		dbus_message_iter_next(&iter);
	}
	/* actions: skip array */
	if (dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_ARRAY) {
		dbus_message_iter_next(&iter);
	}
	/* hints a{sv} */
	if (dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_ARRAY) {
		DBusMessageIter hints_iter, entry_iter;
		dbus_message_iter_recurse(&iter, &hints_iter);
		while (dbus_message_iter_get_arg_type(&hints_iter) ==
		    DBUS_TYPE_DICT_ENTRY) {
			dbus_message_iter_recurse(&hints_iter, &entry_iter);
			const char *key = NULL;
			if (dbus_message_iter_get_arg_type(&entry_iter) ==
			    DBUS_TYPE_STRING) {
				dbus_message_iter_get_basic(&entry_iter, &key);
				dbus_message_iter_next(&entry_iter);
			}
			if (!key) {
				dbus_message_iter_next(&hints_iter);
				continue;
			}
			/* value is VARIANT */
			if (dbus_message_iter_get_arg_type(&entry_iter) ==
			    DBUS_TYPE_VARIANT) {
				DBusMessageIter viter;
				dbus_message_iter_recurse(&entry_iter, &viter);
				if (strcmp(key, "urgency") == 0) {
					if (dbus_message_iter_get_arg_type(&viter) ==
					    DBUS_TYPE_BYTE) {
						uint8_t u = 0;
						dbus_message_iter_get_basic(&viter, &u);
						urgency = (int) u;
						if (urgency < 0 || urgency > 2)
							urgency = 1;
					}
				} else if (strcmp(key, "image-data") == 0 ||
				    strcmp(key, "icon_data") == 0) {
					img_surf = notif_image_data_to_surface(&viter);
				}
			}
			dbus_message_iter_next(&hints_iter);
		}
		dbus_message_iter_next(&iter);
	}
	/* timeout */
	timeout_ms = NOTIF_DEFAULT_TIMEOUT;
	if (dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_INT32) {
		int32_t t = 0;
		dbus_message_iter_get_basic(&iter, &t);
		if (t > 0)
			timeout_ms = (int) t;
		else if (t == -1)
			timeout_ms = NOTIF_DEFAULT_TIMEOUT;
	}

	/* If replaces_id > 0 and we have that notification, close it first */
	if (replaces_id > 0) {
		NotifItem *it;
		for (it = notif_list; it; it = it->next) {
			if (it->id == replaces_id) {
				/* Close silently: don't emit NotificationClosed */
				if (it->timer_id) {
					g_source_remove(it->timer_id);
					it->timer_id = 0;
				}
				if (it->win) {
					gtk_widget_destroy(it->win);
					it->win = NULL;
					it->da  = NULL;
				}
				{
					NotifItem **pp;
					for (pp = &notif_list; *pp; pp = &(*pp)->next) {
						if (*pp == it) {
							*pp = it->next;
							notif_count--;
							break;
						}
					}
				}
				free(it->app_name);
				free(it->summary);
				free(it->body);
				free(it->icon_name);
				if (it->icon_surf)
					cairo_surface_destroy(it->icon_surf);
				free(it);
				break;
			}
		}
	}

	/* Enforce maximum visible — drop oldest if needed */
	while (notif_count >= NOTIF_MAX_VISIBLE && notif_list)
		notif_item_close(notif_list, 3); /* reason 3 = forced close */

	/* Build new notification */
	NotifItem *it = (NotifItem *) calloc(1, sizeof(NotifItem));
	if (!it) {
		if (img_surf)
			cairo_surface_destroy(img_surf);
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	}

	it->id = notif_seq++;
	if (notif_seq == 0)
		notif_seq = 1; /* wrap, skip 0 */
	it->app_name   = strdup(app_name ? app_name : "");
	it->summary    = strdup(summary ? summary : "");
	it->body       = strdup(body ? body : "");
	it->icon_name  = strdup(app_icon ? app_icon : "");
	it->icon_surf  = img_surf;
	it->urgency    = urgency;
	it->timeout_ms = timeout_ms;
	it->next       = NULL;

	/* Append to end of list */
	{
		NotifItem **pp = &notif_list;
		while (*pp)
			pp = &(*pp)->next;
		*pp = it;
	}
	notif_count++;

	notif_item_show(it);
	notif_restack();

	/* Reply with notification id */
	DBusMessage *reply = dbus_message_new_method_return(msg);
	if (reply) {
		uint32_t rid = it->id;
		dbus_message_append_args(
		    reply, DBUS_TYPE_UINT32, &rid, DBUS_TYPE_INVALID);
		dbus_connection_send(conn, reply, NULL);
		dbus_message_unref(reply);
	}
	dbus_connection_flush(conn);
	return DBUS_HANDLER_RESULT_HANDLED;
}

static DBusHandlerResult
handle_close_notification(DBusConnection *conn, DBusMessage *msg)
{
	uint32_t        id = 0;
	DBusMessageIter iter;
	NotifItem      *it;

	dbus_message_iter_init(msg, &iter);
	if (dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_UINT32)
		dbus_message_iter_get_basic(&iter, &id);

	for (it = notif_list; it; it = it->next) {
		if (it->id == id) {
			notif_item_close(it, 3); /* reason 3 = closed by caller */
			break;
		}
	}

	DBusMessage *reply = dbus_message_new_method_return(msg);
	if (reply) {
		dbus_connection_send(conn, reply, NULL);
		dbus_message_unref(reply);
	}
	dbus_connection_flush(conn);
	return DBUS_HANDLER_RESULT_HANDLED;
}

static DBusHandlerResult
handle_get_capabilities(DBusConnection *conn, DBusMessage *msg)
{
	DBusMessage    *reply;
	DBusMessageIter iter, arr;
	const char     *caps[] = {
        "body",
        "body-markup",
        "icon-static",
        "actions",
        "urgency",
	};
	int i;

	reply = dbus_message_new_method_return(msg);
	if (!reply)
		return DBUS_HANDLER_RESULT_NEED_MEMORY;

	dbus_message_iter_init_append(reply, &iter);
	dbus_message_iter_open_container(
	    &iter, DBUS_TYPE_ARRAY, DBUS_TYPE_STRING_AS_STRING, &arr);
	for (i = 0; i < (int) (sizeof(caps) / sizeof(caps[0])); i++)
		dbus_message_iter_append_basic(&arr, DBUS_TYPE_STRING, &caps[i]);
	dbus_message_iter_close_container(&iter, &arr);

	dbus_connection_send(conn, reply, NULL);
	dbus_message_unref(reply);
	dbus_connection_flush(conn);
	return DBUS_HANDLER_RESULT_HANDLED;
}

static DBusHandlerResult
handle_get_server_info(DBusConnection *conn, DBusMessage *msg)
{
	DBusMessage *reply;
	const char  *name    = "awm-notif";
	const char  *vendor  = "andrathwm";
	const char  *version = "1.0";
	const char  *spec    = "1.2";

	reply = dbus_message_new_method_return(msg);
	if (!reply)
		return DBUS_HANDLER_RESULT_NEED_MEMORY;

	dbus_message_append_args(reply, DBUS_TYPE_STRING, &name, DBUS_TYPE_STRING,
	    &vendor, DBUS_TYPE_STRING, &version, DBUS_TYPE_STRING, &spec,
	    DBUS_TYPE_INVALID);

	dbus_connection_send(conn, reply, NULL);
	dbus_message_unref(reply);
	dbus_connection_flush(conn);
	return DBUS_HANDLER_RESULT_HANDLED;
}

/* -------------------------------------------------------------------------
 * D-Bus message filter
 * ---------------------------------------------------------------------- */

static DBusHandlerResult
notif_message_filter(DBusConnection *conn, DBusMessage *msg, void *user_data)
{
	(void) user_data;

	if (dbus_message_get_type(msg) != DBUS_MESSAGE_TYPE_METHOD_CALL)
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

	if (!dbus_message_has_interface(msg, NOTIF_INTERFACE))
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

	if (dbus_message_has_member(msg, "Notify"))
		return handle_notify(conn, msg);
	if (dbus_message_has_member(msg, "CloseNotification"))
		return handle_close_notification(conn, msg);
	if (dbus_message_has_member(msg, "GetCapabilities"))
		return handle_get_capabilities(conn, msg);
	if (dbus_message_has_member(msg, "GetServerInformation"))
		return handle_get_server_info(conn, msg);

	return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

/* -------------------------------------------------------------------------
 * GLib integration — pump D-Bus on a GLib idle source
 * ---------------------------------------------------------------------- */

static guint notif_idle_id = 0;

static gboolean
notif_idle_cb(gpointer data)
{
	(void) data;
	if (notif_conn) {
		dbus_connection_read_write(notif_conn, 0);
		while (
		    dbus_connection_dispatch(notif_conn) == DBUS_DISPATCH_DATA_REMAINS)
			;
	}
	return G_SOURCE_CONTINUE;
}

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

int
notif_init(int mon_wx, int mon_wy, int mon_ww, int mon_wh)
{
	DBusError err;

	notif_mon_wx = mon_wx;
	notif_mon_wy = mon_wy;
	notif_mon_ww = mon_ww;
	notif_mon_wh = mon_wh;

	dbus_error_init(&err);
	notif_conn = dbus_bus_get(DBUS_BUS_SESSION, &err);
	if (!notif_conn) {
		awm_error("notif: dbus_bus_get: %s", err.message);
		dbus_error_free(&err);
		return -1;
	}

	int ret = dbus_bus_request_name(
	    notif_conn, NOTIF_BUS_NAME, DBUS_NAME_FLAG_REPLACE_EXISTING, &err);
	if (dbus_error_is_set(&err)) {
		awm_error("notif: request_name: %s", err.message);
		dbus_error_free(&err);
		dbus_connection_unref(notif_conn);
		notif_conn = NULL;
		return -1;
	}
	if (ret != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER) {
		awm_warn("notif: could not become primary owner of %s (ret=%d)",
		    NOTIF_BUS_NAME, ret);
		/* Continue anyway — we may be replacing another daemon */
	}

	dbus_connection_add_filter(notif_conn, notif_message_filter, NULL, NULL);

	/* Pump D-Bus every ~50 ms via a GLib timeout (low priority) */
	notif_idle_id = g_timeout_add(50, notif_idle_cb, NULL);

	awm_info("notif: registered %s on session bus", NOTIF_BUS_NAME);
	return 0;
}

void
notif_update_geom(int mon_wx, int mon_wy, int mon_ww, int mon_wh)
{
	NotifItem *it;

	notif_mon_wx   = mon_wx;
	notif_mon_wy   = mon_wy;
	notif_mon_ww   = mon_ww;
	notif_mon_wh   = mon_wh;
	notif_geom_set = 1;

	/* Show any popups that arrived before geometry was known */
	for (it = notif_list; it; it = it->next) {
		if (!it->win)
			notif_item_show(it);
	}

	notif_restack();
}

void
notif_update_theme(const UiThemePayload *t)
{
	NotifItem *it;

	if (!t)
		return;
	notif_theme     = *t;
	notif_theme_set = 1;

	/* Force a redraw of every visible popup */
	for (it = notif_list; it; it = it->next) {
		if (it->da)
			gtk_widget_queue_draw(it->da);
	}
}

void
notif_dispatch(void)
{
	if (!notif_conn)
		return;
	dbus_connection_read_write(notif_conn, 0);
	while (dbus_connection_dispatch(notif_conn) == DBUS_DISPATCH_DATA_REMAINS)
		;
}

void
notif_cleanup(void)
{
	if (notif_idle_id) {
		g_source_remove(notif_idle_id);
		notif_idle_id = 0;
	}

	/* Close all open notifications silently */
	while (notif_list) {
		NotifItem *it = notif_list;
		notif_list    = it->next;
		notif_count--;
		if (it->timer_id) {
			g_source_remove(it->timer_id);
			it->timer_id = 0;
		}
		if (it->win) {
			gtk_widget_destroy(it->win);
			it->win = NULL;
		}
		free(it->app_name);
		free(it->summary);
		free(it->body);
		free(it->icon_name);
		if (it->icon_surf)
			cairo_surface_destroy(it->icon_surf);
		free(it);
	}

	if (notif_conn) {
		dbus_connection_unref(notif_conn);
		notif_conn = NULL;
	}
}
