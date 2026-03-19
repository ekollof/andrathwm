/* See LICENSE file for copyright and license details. */
/* platform_x11.c -- X11 WmBackend vtable implementation.
 *
 * g_plat is zero-initialised at program start.  All fields are populated
 * by setup() in awm.c before any other module reads them.
 *
 * Every static function here is the concrete implementation of one slot in
 * the WmBackend vtable.  wm_backend_x11 is the sole instance; g_wm_backend
 * is set to &wm_backend_x11 as the very first action of setup().
 */

#include <assert.h>
#include <string.h>
#include <stdlib.h>
#include <xcb/xcb.h>
#include <xcb/xcb_icccm.h>
#include <xcb/xcb_keysyms.h>
#include <xkbcommon/xkbcommon-keysyms.h>
#ifdef XRANDR
#include <xcb/randr.h>
#endif

#include "awm.h"
#include "client.h"
#include "compositor.h"
#include "events.h"
#include "monitor.h"
#include "spawn.h"
#include "switcher.h"
#include "wmstate.h"
#include "xrdb.h"
#include "platform.h"
#include "config.h"

/* -------------------------------------------------------------------------
 * connection
 * ---------------------------------------------------------------------- */

static void
x11_flush(PlatformCtx *p)
{
	xcb_flush(p->xc);
}

/* -------------------------------------------------------------------------
 * window geometry & stacking
 * ---------------------------------------------------------------------- */

static void
x11_configure_win(PlatformCtx *p, WinId w, uint16_t mask, const uint32_t *vals)
{
	xcb_configure_window(p->xc, (xcb_window_t) w, mask, vals);
}

/* -------------------------------------------------------------------------
 * window attributes
 * ---------------------------------------------------------------------- */

static void
x11_change_attr(PlatformCtx *p, WinId w, uint32_t mask, const uint32_t *vals)
{
	xcb_change_window_attributes(p->xc, (xcb_window_t) w, mask, vals);
}

/* -------------------------------------------------------------------------
 * window visibility
 * ---------------------------------------------------------------------- */

static void
x11_map(PlatformCtx *p, WinId w)
{
	xcb_map_window(p->xc, (xcb_window_t) w);
}

static void
x11_unmap(PlatformCtx *p, WinId w)
{
	xcb_unmap_window(p->xc, (xcb_window_t) w);
}

static void
x11_destroy_win(PlatformCtx *p, WinId w)
{
	xcb_destroy_window(p->xc, (xcb_window_t) w);
}

/* -------------------------------------------------------------------------
 * window events
 * ---------------------------------------------------------------------- */

static void
x11_send_configure_notify(
    PlatformCtx *p, WinId w, int x, int y, int bw, int ww, int wh)
{
	xcb_configure_notify_event_t ce;

	ce.response_type     = XCB_CONFIGURE_NOTIFY;
	ce.pad0              = 0;
	ce.sequence          = 0;
	ce.event             = (xcb_window_t) w;
	ce.window            = (xcb_window_t) w;
	ce.above_sibling     = XCB_NONE;
	ce.x                 = (int16_t) x;
	ce.y                 = (int16_t) y;
	ce.width             = (uint16_t) ww;
	ce.height            = (uint16_t) wh;
	ce.border_width      = (uint16_t) bw;
	ce.override_redirect = 0;
	ce.pad1              = 0;
	xcb_send_event(p->xc, 0, (xcb_window_t) w, XCB_EVENT_MASK_STRUCTURE_NOTIFY,
	    (const char *) &ce);
}

/* -------------------------------------------------------------------------
 * focus & input
 * ---------------------------------------------------------------------- */

static void
x11_set_input_focus(PlatformCtx *p, WinId w, WmTimestamp t)
{
	xcb_set_input_focus(p->xc, XCB_INPUT_FOCUS_POINTER_ROOT, (xcb_window_t) w,
	    (xcb_timestamp_t) t);
}

static void
x11_warp_pointer(PlatformCtx *p, WinId dst, int16_t x, int16_t y)
{
	xcb_warp_pointer(
	    p->xc, XCB_WINDOW_NONE, (xcb_window_t) dst, 0, 0, 0, 0, x, y);
}

static void
x11_allow_events(PlatformCtx *p, int mode, WmTimestamp t)
{
	xcb_allow_events(p->xc, (uint8_t) mode, (xcb_timestamp_t) t);
}

/* -------------------------------------------------------------------------
 * pointer grab
 * ---------------------------------------------------------------------- */

static int
x11_grab_pointer(PlatformCtx *p, WmCursor cur)
{
	xcb_grab_pointer_cookie_t ck = xcb_grab_pointer(p->xc, 0, p->root,
	    MOUSEMASK, XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC, XCB_WINDOW_NONE,
	    (xcb_cursor_t) cur, XCB_CURRENT_TIME);
	xcb_grab_pointer_reply_t *r  = xcb_grab_pointer_reply(p->xc, ck, NULL);
	int                       ok = r && r->status == XCB_GRAB_STATUS_SUCCESS;

	free(r);
	return ok;
}

static void
x11_ungrab_pointer(PlatformCtx *p)
{
	xcb_ungrab_pointer(p->xc, XCB_CURRENT_TIME);
}

static void
x11_ungrab_button(PlatformCtx *p, WinId w)
{
	xcb_ungrab_button(
	    p->xc, XCB_BUTTON_INDEX_ANY, (xcb_window_t) w, XCB_MOD_MASK_ANY);
}

static int
x11_query_pointer(PlatformCtx *p, int *x, int *y)
{
	xcb_query_pointer_cookie_t ck = xcb_query_pointer(p->xc, p->root);
	xcb_query_pointer_reply_t *r  = xcb_query_pointer_reply(p->xc, ck, NULL);

	if (!r)
		return 0;
	*x = r->root_x;
	*y = r->root_y;
	free(r);
	return 1;
}

/* -------------------------------------------------------------------------
 * server grab
 * ---------------------------------------------------------------------- */

static void
x11_grab_server(PlatformCtx *p)
{
	xcb_grab_server(p->xc);
}

static void
x11_ungrab_server(PlatformCtx *p)
{
	xcb_ungrab_server(p->xc);
}

/* -------------------------------------------------------------------------
 * window attributes query
 * ---------------------------------------------------------------------- */

static int
x11_get_override_redirect(PlatformCtx *p, WinId w, int *out)
{
	xcb_get_window_attributes_cookie_t ck =
	    xcb_get_window_attributes(p->xc, (xcb_window_t) w);
	xcb_get_window_attributes_reply_t *r =
	    xcb_get_window_attributes_reply(p->xc, ck, NULL);

	if (!r)
		return 0;
	*out = r->override_redirect;
	free(r);
	return 1;
}

static int
x11_get_event_mask(PlatformCtx *p, WinId w, uint32_t *out)
{
	xcb_get_window_attributes_cookie_t ck =
	    xcb_get_window_attributes(p->xc, (xcb_window_t) w);
	xcb_get_window_attributes_reply_t *r =
	    xcb_get_window_attributes_reply(p->xc, ck, NULL);

	if (!r)
		return 0;
	*out = r->your_event_mask;
	free(r);
	return 1;
}

/* -------------------------------------------------------------------------
 * window geometry
 * ---------------------------------------------------------------------- */

static int
x11_get_geometry(
    PlatformCtx *p, WinId w, int *x, int *y, int *ww, int *wh, int *bw)
{
	xcb_get_geometry_cookie_t ck = xcb_get_geometry(p->xc, (xcb_window_t) w);
	xcb_get_geometry_reply_t *r  = xcb_get_geometry_reply(p->xc, ck, NULL);

	if (!r)
		return 0;
	*x  = r->x;
	*y  = r->y;
	*ww = r->width;
	*wh = r->height;
	*bw = r->border_width;
	free(r);
	return 1;
}

/* -------------------------------------------------------------------------
 * window tree
 * ---------------------------------------------------------------------- */

static int
x11_is_window_descendant(PlatformCtx *p, WinId w, WinId ancestor)
{
	xcb_window_t xw  = (xcb_window_t) w;
	xcb_window_t xan = (xcb_window_t) ancestor;

	while (xw && xw != xan && xw != p->root) {
		xcb_query_tree_reply_t *r =
		    xcb_query_tree_reply(p->xc, xcb_query_tree(p->xc, xw), NULL);
		if (!r)
			break;
		xw = r->parent;
		free(r);
	}
	return xw == xan;
}

/* -------------------------------------------------------------------------
 * property reads
 * ---------------------------------------------------------------------- */

static AtomId
x11_get_atom_prop(PlatformCtx *p, WinId w, AtomId prop, AtomId type)
{
	xcb_get_property_cookie_t ck = xcb_get_property(p->xc, 0, (xcb_window_t) w,
	    (xcb_atom_t) prop, (xcb_atom_t) type, 0, 1);
	xcb_get_property_reply_t *r  = xcb_get_property_reply(p->xc, ck, NULL);
	AtomId                    atom = ATOM_NONE;

	if (r && xcb_get_property_value_length(r) >= (int) sizeof(xcb_atom_t))
		atom = (AtomId) * (xcb_atom_t *) xcb_get_property_value(r);
	free(r);
	return atom;
}

static int
x11_get_text_prop(
    PlatformCtx *p, WinId w, AtomId atom, char *buf, unsigned int size)
{
	xcb_get_property_cookie_t           ck;
	xcb_icccm_get_text_property_reply_t prop;
	unsigned int                        len;

	if (!buf || size == 0)
		return 0;
	buf[0] = '\0';
	ck     = xcb_icccm_get_text_property(
        p->xc, (xcb_window_t) w, (xcb_atom_t) atom);
	if (!xcb_icccm_get_text_property_reply(p->xc, ck, &prop, NULL))
		return 0;
	if (prop.name_len > 0 && prop.name) {
		len = prop.name_len < size - 1 ? prop.name_len : size - 1;
		memcpy(buf, prop.name, len);
		buf[len] = '\0';
	}
	xcb_icccm_get_text_property_reply_wipe(&prop);
	return 1;
}

static int
x11_get_wm_icon(PlatformCtx *p, WinId w, uint32_t **data_out, int *nitems_out)
{
	xcb_get_property_cookie_t ck = xcb_get_property(p->xc, 0, (xcb_window_t) w,
	    p->netatom[NetWMIcon], XCB_ATOM_ANY, 0, UINT32_MAX / 4);
	xcb_get_property_reply_t *r  = xcb_get_property_reply(p->xc, ck, NULL);
	int                       n;
	uint32_t                 *copy;

	if (!r || xcb_get_property_value_length(r) == 0) {
		free(r);
		return 0;
	}
	n    = xcb_get_property_value_length(r) / (int) sizeof(uint32_t);
	copy = malloc((size_t) n * sizeof(uint32_t));
	if (!copy) {
		free(r);
		return 0;
	}
	memcpy(copy, xcb_get_property_value(r), (size_t) n * sizeof(uint32_t));
	free(r);
	*data_out   = copy;
	*nitems_out = n;
	return 1;
}

static int
x11_get_wm_class(PlatformCtx *p, WinId w, char *inst_buf,
    unsigned int inst_size, char *cls_buf, unsigned int cls_size)
{
	xcb_get_property_cookie_t ck = xcb_get_property(p->xc, 0, (xcb_window_t) w,
	    XCB_ATOM_WM_CLASS, XCB_ATOM_STRING, 0, 512);
	xcb_get_property_reply_t *r  = xcb_get_property_reply(p->xc, ck, NULL);

	if (!r || xcb_get_property_value_length(r) == 0) {
		free(r);
		return 0;
	}
	{
		const char *val = (const char *) xcb_get_property_value(r);
		int         len = xcb_get_property_value_length(r);
		int         il  = (int) strnlen(val, (size_t) len);

		if (il > 0)
			snprintf(inst_buf, (size_t) inst_size, "%.*s", il, val);
		else
			inst_buf[0] = '\0';

		if (il + 1 < len) {
			const char *cs = val + il + 1;
			int         cl = (int) strnlen(cs, (size_t) (len - il - 1));

			if (cl > 0)
				snprintf(cls_buf, (size_t) cls_size, "%.*s", cl, cs);
			else
				cls_buf[0] = '\0';
		} else {
			cls_buf[0] = '\0';
		}
	}
	free(r);
	return 1;
}

/* -------------------------------------------------------------------------
 * property writes
 * ---------------------------------------------------------------------- */

static void
x11_change_prop(PlatformCtx *p, WinId w, AtomId prop, AtomId type, int format,
    uint8_t mode, int n, const void *data)
{
	xcb_change_property(p->xc, mode, (xcb_window_t) w, (xcb_atom_t) prop,
	    (xcb_atom_t) type, (uint8_t) format, (uint32_t) n, data);
}

static void
x11_delete_prop(PlatformCtx *p, WinId w, AtomId prop)
{
	xcb_delete_property(p->xc, (xcb_window_t) w, (xcb_atom_t) prop);
}

/* -------------------------------------------------------------------------
 * kill / close
 * ---------------------------------------------------------------------- */

static void
x11_kill_client_hard(PlatformCtx *p, WinId w)
{
	xcb_grab_server(p->xc);
	xcb_set_close_down_mode(p->xc, XCB_CLOSE_DOWN_DESTROY_ALL);
	xcb_kill_client(p->xc, (xcb_window_t) w);
	xcb_ungrab_server(p->xc);
}

/* -------------------------------------------------------------------------
 * save-set / reparent (systray)
 * ---------------------------------------------------------------------- */

static void
x11_change_save_set(PlatformCtx *p, WinId w, int insert)
{
	xcb_change_save_set(p->xc,
	    insert ? XCB_SET_MODE_INSERT : XCB_SET_MODE_DELETE, (xcb_window_t) w);
}

static void
x11_reparent_window(PlatformCtx *p, WinId w, WinId parent, int x, int y)
{
	xcb_reparent_window(p->xc, (xcb_window_t) w, (xcb_window_t) parent,
	    (int16_t) x, (int16_t) y);
}

/* -------------------------------------------------------------------------
 * bar window creation
 * ---------------------------------------------------------------------- */

static WinId
x11_create_bar_win(
    PlatformCtx *p, int x, int y, int w, int h, int compositor_active)
{
	int          depth  = xcb_screen_root_depth(p->xc, p->screen);
	xcb_window_t barwin = xcb_generate_id(p->xc);

#ifdef COMPOSITOR
	if (compositor_active) {
		uint32_t vals[3] = {
			(uint32_t) scheme[SchemeNorm][ColBg].pixel,
			1,
			XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_EXPOSURE |
			    XCB_EVENT_MASK_ENTER_WINDOW | XCB_EVENT_MASK_LEAVE_WINDOW,
		};
		xcb_create_window(p->xc, (uint8_t) depth, barwin, p->root, (int16_t) x,
		    (int16_t) y, (uint16_t) w, (uint16_t) h, 0,
		    XCB_WINDOW_CLASS_INPUT_OUTPUT, XCB_COPY_FROM_PARENT,
		    XCB_CW_BACK_PIXEL | XCB_CW_OVERRIDE_REDIRECT | XCB_CW_EVENT_MASK,
		    vals);
		return barwin;
	}
#else
	(void) compositor_active;
#endif
	{
		uint32_t vals[3] = {
			XCB_BACK_PIXMAP_PARENT_RELATIVE,
			1,
			XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_EXPOSURE |
			    XCB_EVENT_MASK_ENTER_WINDOW | XCB_EVENT_MASK_LEAVE_WINDOW,
		};
		xcb_create_window(p->xc, (uint8_t) depth, barwin, p->root, (int16_t) x,
		    (int16_t) y, (uint16_t) w, (uint16_t) h, 0,
		    XCB_WINDOW_CLASS_INPUT_OUTPUT, XCB_COPY_FROM_PARENT,
		    XCB_CW_BACK_PIXMAP | XCB_CW_OVERRIDE_REDIRECT | XCB_CW_EVENT_MASK,
		    vals);
	}
	return barwin;
}

static int
x11_screen_depth(PlatformCtx *p)
{
	return xcb_screen_root_depth(p->xc, p->screen);
}

/* -------------------------------------------------------------------------
 * ICCCM helpers
 * ---------------------------------------------------------------------- */

static int
x11_get_wm_normal_hints(PlatformCtx *p, WinId w, AwmSizeHints *out)
{
	xcb_size_hints_t          sh;
	xcb_get_property_cookie_t ck;
	int                       ok;

	ck = xcb_icccm_get_wm_normal_hints(p->xc, (xcb_window_t) w);
	ok = xcb_icccm_get_wm_normal_hints_reply(p->xc, ck, &sh, NULL);
	if (!ok)
		return 0;

	out->flags      = 0;
	out->min_w      = sh.min_width;
	out->min_h      = sh.min_height;
	out->max_w      = sh.max_width;
	out->max_h      = sh.max_height;
	out->base_w     = sh.base_width;
	out->base_h     = sh.base_height;
	out->inc_w      = sh.width_inc;
	out->inc_h      = sh.height_inc;
	out->min_aspect = (sh.min_aspect_num && sh.min_aspect_den)
	    ? (double) sh.min_aspect_num / (double) sh.min_aspect_den
	    : 0.0;
	out->max_aspect = (sh.max_aspect_num && sh.max_aspect_den)
	    ? (double) sh.max_aspect_num / (double) sh.max_aspect_den
	    : 0.0;

	if (sh.flags & XCB_ICCCM_SIZE_HINT_P_MIN_SIZE)
		out->flags |= AWM_SIZE_P_MIN_SIZE;
	if (sh.flags & XCB_ICCCM_SIZE_HINT_P_MAX_SIZE)
		out->flags |= AWM_SIZE_P_MAX_SIZE;
	if (sh.flags & XCB_ICCCM_SIZE_HINT_P_RESIZE_INC)
		out->flags |= AWM_SIZE_P_RESIZE_INC;
	if (sh.flags & XCB_ICCCM_SIZE_HINT_P_ASPECT)
		out->flags |= AWM_SIZE_P_ASPECT;
	if (sh.flags & XCB_ICCCM_SIZE_HINT_BASE_SIZE)
		out->flags |= AWM_SIZE_P_BASE_SIZE;
	if (sh.flags & XCB_ICCCM_SIZE_HINT_P_SIZE)
		out->flags |= AWM_SIZE_P_SIZE;
	return 1;
}

static int
x11_get_wm_hints(PlatformCtx *p, WinId w, AwmWmHints *out)
{
	xcb_icccm_wm_hints_t      wmh;
	xcb_get_property_cookie_t ck;
	int                       ok;

	ck = xcb_icccm_get_wm_hints(p->xc, (xcb_window_t) w);
	ok = xcb_icccm_get_wm_hints_reply(p->xc, ck, &wmh, NULL);
	if (!ok)
		return 0;

	out->flags = 0;
	out->input = wmh.input;
	if (wmh.flags & XCB_ICCCM_WM_HINT_INPUT)
		out->flags |= AWM_WM_HINT_INPUT;
	if (wmh.flags & XCB_ICCCM_WM_HINT_X_URGENCY)
		out->flags |= AWM_WM_HINT_URGENCY;
	return 1;
}

static void
x11_set_wm_hints(PlatformCtx *p, WinId w, const AwmWmHints *hints)
{
	xcb_icccm_wm_hints_t wmh;

	memset(&wmh, 0, sizeof wmh);
	wmh.flags = 0;
	wmh.input = (uint32_t) hints->input;
	if (hints->flags & AWM_WM_HINT_INPUT)
		wmh.flags |= XCB_ICCCM_WM_HINT_INPUT;
	if (hints->flags & AWM_WM_HINT_URGENCY)
		wmh.flags |= XCB_ICCCM_WM_HINT_X_URGENCY;
	xcb_icccm_set_wm_hints(p->xc, (xcb_window_t) w, &wmh);
}

static WinId
x11_get_wm_transient_for(PlatformCtx *p, WinId w)
{
	xcb_window_t trans = XCB_WINDOW_NONE;

	xcb_icccm_get_wm_transient_for_reply(p->xc,
	    xcb_icccm_get_wm_transient_for(p->xc, (xcb_window_t) w), &trans, NULL);
	return (WinId) trans;
}

/* -------------------------------------------------------------------------
 * compound / higher-level operations
 * ---------------------------------------------------------------------- */

static void
x11_grab_buttons(PlatformCtx *p, WinId w, int focused)
{
	unsigned int i, j;
	unsigned int modifiers[] = { 0, XCB_MOD_MASK_LOCK, p->numlockmask,
		p->numlockmask | XCB_MOD_MASK_LOCK };

	xcb_ungrab_button(
	    p->xc, XCB_BUTTON_INDEX_ANY, (xcb_window_t) w, XCB_MOD_MASK_ANY);
	if (!focused)
		xcb_grab_button(p->xc, 0, (xcb_window_t) w,
		    XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_BUTTON_RELEASE,
		    XCB_GRAB_MODE_SYNC, XCB_GRAB_MODE_SYNC, XCB_WINDOW_NONE,
		    XCB_CURSOR_NONE, XCB_BUTTON_INDEX_ANY, XCB_MOD_MASK_ANY);
	for (i = 0; i < LENGTH(buttons); i++)
		if (buttons[i].click == ClkClientWin)
			for (j = 0; j < LENGTH(modifiers); j++)
				xcb_grab_button(p->xc, 0, (xcb_window_t) w,
				    XCB_EVENT_MASK_BUTTON_PRESS |
				        XCB_EVENT_MASK_BUTTON_RELEASE,
				    XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_SYNC, XCB_WINDOW_NONE,
				    XCB_CURSOR_NONE, (uint8_t) buttons[i].button,
				    (uint16_t) (buttons[i].mask | modifiers[j]));
}

static void
x11_update_numlock_mask(PlatformCtx *p)
{
	unsigned int                      i, j;
	xcb_get_modifier_mapping_cookie_t ck = xcb_get_modifier_mapping(p->xc);
	xcb_get_modifier_mapping_reply_t *mr;
	xcb_keycode_t                    *nlcodes;
	xcb_keycode_t                    *modcodes;

	p->numlockmask = 0;
	mr             = xcb_get_modifier_mapping_reply(p->xc, ck, NULL);
	if (!mr)
		return;
	nlcodes  = xcb_key_symbols_get_keycode(p->keysyms, XKB_KEY_Num_Lock);
	modcodes = xcb_get_modifier_mapping_keycodes(mr);
	if (nlcodes) {
		for (i = 0; i < 8; i++)
			for (j = 0; j < mr->keycodes_per_modifier; j++) {
				xcb_keycode_t kc = modcodes[i * mr->keycodes_per_modifier + j];
				xcb_keycode_t *nl;

				for (nl = nlcodes; *nl != XCB_NO_SYMBOL; nl++)
					if (kc == *nl)
						p->numlockmask |= (1u << i);
			}
		free(nlcodes);
	}
	free(mr);
}

static void
x11_grab_keys_full(PlatformCtx *p)
{
	unsigned int                      i, j, k;
	unsigned int                      modifiers[4];
	const xcb_setup_t                *setup;
	xcb_keycode_t                     kmin, kmax;
	int                               count, skip;
	xcb_get_keyboard_mapping_cookie_t mck;
	xcb_get_keyboard_mapping_reply_t *mr;
	xcb_keysym_t                     *syms;

	x11_update_numlock_mask(p);
	modifiers[0] = 0;
	modifiers[1] = XCB_MOD_MASK_LOCK;
	modifiers[2] = p->numlockmask;
	modifiers[3] = p->numlockmask | XCB_MOD_MASK_LOCK;

	setup = xcb_get_setup(p->xc);
	kmin  = setup->min_keycode;
	kmax  = setup->max_keycode;
	count = kmax - kmin + 1;

	xcb_ungrab_key(p->xc, XCB_GRAB_ANY, p->root, XCB_MOD_MASK_ANY);
	mck = xcb_get_keyboard_mapping(p->xc, kmin, (uint8_t) count);
	mr  = xcb_get_keyboard_mapping_reply(p->xc, mck, NULL);
	if (!mr)
		return;
	skip = mr->keysyms_per_keycode;
	syms = xcb_get_keyboard_mapping_keysyms(mr);
	for (k = kmin; k <= kmax; k++)
		for (i = 0; i < LENGTH(keys); i++)
			if (keys[i].keysym == (KeySym) syms[(k - kmin) * skip])
				for (j = 0; j < LENGTH(modifiers); j++)
					xcb_grab_key(p->xc, 1, p->root,
					    (uint16_t) (keys[i].mod | modifiers[j]),
					    (xcb_keycode_t) k, XCB_GRAB_MODE_ASYNC,
					    XCB_GRAB_MODE_ASYNC);
	free(mr);
}

static void
x11_refresh_keyboard_mapping(PlatformCtx *p, AwmEvent *ev)
{
	xcb_mapping_notify_event_t xme;

	memset(&xme, 0, sizeof xme);
	xme.request       = ev->mapping.request;
	xme.first_keycode = ev->mapping.first_keycode;
	xme.count         = ev->mapping.count;
	xcb_refresh_keyboard_mapping(p->keysyms, &xme);
}

static KeySym
x11_get_keysym(PlatformCtx *p, WmKeycode code, int col)
{
	return (KeySym) xcb_key_symbols_get_keysym(
	    p->keysyms, (xcb_keycode_t) code, col);
}

static void
x11_check_other_wm(PlatformCtx *p)
{
	uint32_t             mask = XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT;
	xcb_void_cookie_t    ck;
	xcb_generic_error_t *err;

	if (!p->root) {
		xcb_screen_iterator_t sit =
		    xcb_setup_roots_iterator(xcb_get_setup(p->xc));
		int i;

		for (i = 0; i < p->screen; i++)
			xcb_screen_next(&sit);
		p->root = sit.data->root;
	}
	ck = xcb_change_window_attributes_checked(
	    p->xc, p->root, XCB_CW_EVENT_MASK, &mask);
	err = xcb_request_check(p->xc, ck);
	if (err) {
		free(err);
		die("awm: another window manager is already running");
	}
}

#ifdef XINERAMA
static int
isuniquegeom(xcb_xinerama_screen_info_t *unique, size_t n,
    xcb_xinerama_screen_info_t *info)
{
	while (n--)
		if (unique[n].x_org == info->x_org && unique[n].y_org == info->y_org &&
		    unique[n].width == info->width && unique[n].height == info->height)
			return 0;
	return 1;
}
#endif /* XINERAMA */

static int
x11_update_geom(PlatformCtx *p)
{
	int dirty = 0;

#ifdef XRANDR
	{
		const xcb_query_extension_reply_t *ext =
		    xcb_get_extension_data(p->xc, &xcb_randr_id);
		if (ext && ext->present) {
			int                                             i, n, nn;
			Client                                         *c;
			xcb_randr_get_screen_resources_current_cookie_t src;
			xcb_randr_get_screen_resources_current_reply_t *sr;
			xcb_randr_crtc_t                               *crtcs;
			typedef struct {
				int x, y, w, h;
			} ScreenGeom;
			ScreenGeom *unique = NULL;
			int         j;

			src = xcb_randr_get_screen_resources_current(p->xc, p->root);
			sr =
			    xcb_randr_get_screen_resources_current_reply(p->xc, src, NULL);
			if (!sr)
				goto xinerama_fallback;

			crtcs  = xcb_randr_get_screen_resources_current_crtcs(sr);
			nn     = 0;
			unique = ecalloc(sr->num_crtcs, sizeof(ScreenGeom));
			/* Get active CRTC geometries */
			for (i = 0; i < (int) sr->num_crtcs; i++) {
				xcb_randr_get_crtc_info_cookie_t cic =
				    xcb_randr_get_crtc_info(p->xc, crtcs[i], XCB_CURRENT_TIME);
				xcb_randr_get_crtc_info_reply_t *ci =
				    xcb_randr_get_crtc_info_reply(p->xc, cic, NULL);
				if (!ci || ci->num_outputs == 0 || ci->width == 0) {
					free(ci);
					continue;
				}
				/* Check if geometry is unique */
				int is_unique = 1;
				for (j = 0; j < nn; j++) {
					if (unique[j].x == (int) ci->x &&
					    unique[j].y == (int) ci->y &&
					    unique[j].w == (int) ci->width &&
					    unique[j].h == (int) ci->height) {
						is_unique = 0;
						break;
					}
				}
				if (is_unique) {
					unique[nn].x = ci->x;
					unique[nn].y = ci->y;
					unique[nn].w = ci->width;
					unique[nn].h = ci->height;
					nn++;
				}
				free(ci);
			}
			free(sr);

			n = (int) g_awm.n_monitors;

			/* Create new monitors if nn > n */
			for (i = n; i < nn; i++) {
				if (!createmon())
					die("awm: createmon failed: monitor count "
					    "exceeds tag count");
			}

			/* Update monitor geometries */
			for (i = 0; i < nn; i++) {
				Monitor *m = &g_awm.monitors[i];
				if (i >= n || unique[i].x != m->mx || unique[i].y != m->my ||
				    unique[i].w != m->mw || unique[i].h != m->mh) {
					dirty  = 1;
					m->num = i;
					m->mx = m->wx = unique[i].x;
					m->my = m->wy = unique[i].y;
					m->mw = m->ww = unique[i].w;
					m->mh = m->wh = unique[i].h;
					updatebarpos(m);
				}
			}

			/* Remove monitors if n > nn — remove from the tail. */
			while ((int) g_awm.n_monitors > nn) {
				Monitor *dying = &g_awm.monitors[g_awm.n_monitors - 1];
				/* Redirect selmon and clients away from dying. */
				if (g_awm.selmon_num == (int) (g_awm.n_monitors - 1))
					g_awm.selmon_num = 0;
				for (c = g_awm.clients_head; c; c = c->next) {
					dirty = 1;
					if (c->mon == dying)
						c->mon = g_awm_selmon;
				}
				cleanupmon(dying);
			}
			free(unique);
			goto geom_done;
		}
	}
xinerama_fallback:
#endif /* XRANDR */
#ifdef XINERAMA
{
	xcb_xinerama_is_active_reply_t *ia = xcb_xinerama_is_active_reply(
	    p->xc, xcb_xinerama_is_active(p->xc), NULL);
	int xin_active = ia && ia->state;
	free(ia);
	if (xin_active) {
		int                                 i, j, n, nn;
		Client                             *c;
		xcb_xinerama_query_screens_reply_t *qi =
		    xcb_xinerama_query_screens_reply(
		        p->xc, xcb_xinerama_query_screens(p->xc), NULL);
		xcb_xinerama_screen_info_t *info =
		    xcb_xinerama_query_screens_screen_info(qi);
		nn = xcb_xinerama_query_screens_screen_info_length(qi);
		xcb_xinerama_screen_info_t *unique = NULL;

		if (!qi || !info) {
			free(qi);
			goto default_monitor;
		}
		n = (int) g_awm.n_monitors;
		/* only consider unique geometries as separate screens */
		unique = ecalloc((size_t) nn, sizeof(xcb_xinerama_screen_info_t));
		for (i = 0, j = 0; i < nn; i++)
			if (isuniquegeom(unique, j, &info[i]))
				memcpy(&unique[j++], &info[i],
				    sizeof(xcb_xinerama_screen_info_t));
		free(qi); /* frees info array too */
		nn = j;

		/* new monitors if nn > n */
		for (i = n; i < nn; i++) {
			if (!createmon())
				die("awm: createmon failed: monitor count "
				    "exceeds tag count");
		}
		for (i = 0; i < nn; i++) {
			Monitor *m = &g_awm.monitors[i];
			if (i >= n || unique[i].x_org != m->mx ||
			    unique[i].y_org != m->my || unique[i].width != m->mw ||
			    unique[i].height != m->mh) {
				dirty  = 1;
				m->num = i;
				m->mx = m->wx = unique[i].x_org;
				m->my = m->wy = unique[i].y_org;
				m->mw = m->ww = unique[i].width;
				m->mh = m->wh = unique[i].height;
				updatebarpos(m);
			}
		}
		/* removed monitors if n > nn — remove from tail */
		while ((int) g_awm.n_monitors > nn) {
			Monitor *dying = &g_awm.monitors[g_awm.n_monitors - 1];
			if (g_awm.selmon_num == (int) (g_awm.n_monitors - 1))
				g_awm.selmon_num = 0;
			for (c = g_awm.clients_head; c; c = c->next) {
				dirty = 1;
				if (c->mon == dying)
					c->mon = g_awm_selmon;
			}
			cleanupmon(dying);
		}
		free(unique);
		goto geom_done;
	}
default_monitor:;
}
#else
default_monitor:
#endif /* XINERAMA */
	/* default monitor setup */
	if (g_awm.n_monitors == 0) {
		if (!createmon())
			die("awm: createmon failed: monitor count exceeds tag "
			    "count");
	}
	if (g_awm.monitors[0].mw != p->sw || g_awm.monitors[0].mh != p->sh) {
		dirty                = 1;
		g_awm.monitors[0].mw = g_awm.monitors[0].ww = p->sw;
		g_awm.monitors[0].mh = g_awm.monitors[0].wh = p->sh;
		updatebarpos(&g_awm.monitors[0]);
	}
geom_done:
	if (dirty)
		g_awm_set_selmon(wintomon(p->root));
	assert(g_awm.n_monitors >= 0 && g_awm.n_monitors <= WMSTATE_MAX_MONITORS);
	wmstate_update();
	return dirty;
}

/* -------------------------------------------------------------------------
 * event loop helpers
 * ---------------------------------------------------------------------- */

/* Translate an xcb_generic_event_t into an AwmEvent.
 * Returns 1 if the event type is known, 0 if it should be ignored.
 * Callers must free xe after use; the returned ev is populated in-place. */
static int
awm_event_from_xcb(xcb_generic_event_t *xe, AwmEvent *ev)
{
	uint8_t type = xe->response_type & ~0x80;

	memset(ev, 0, sizeof *ev);

	switch (type) {
	case XCB_BUTTON_PRESS:
	case XCB_BUTTON_RELEASE: {
		xcb_button_press_event_t *e = (xcb_button_press_event_t *) xe;
		ev->type           = (type == XCB_BUTTON_PRESS) ? AWM_EV_BUTTON_PRESS
		                                                : AWM_EV_BUTTON_RELEASE;
		ev->window         = (WinId) e->event;
		ev->button.button  = e->detail;
		ev->button.state   = (WmMods) e->state;
		ev->button.time    = (WmTimestamp) e->time;
		ev->button.root_x  = e->root_x;
		ev->button.root_y  = e->root_y;
		ev->button.event_x = e->event_x;
		ev->button.event_y = e->event_y;
		break;
	}
	case XCB_KEY_PRESS:
	case XCB_KEY_RELEASE: {
		xcb_key_press_event_t *e = (xcb_key_press_event_t *) xe;
		ev->type =
		    (type == XCB_KEY_PRESS) ? AWM_EV_KEY_PRESS : AWM_EV_KEY_RELEASE;
		ev->window      = (WinId) e->event;
		ev->key.keycode = (WmKeycode) e->detail;
		ev->key.state   = (WmMods) e->state;
		ev->key.time    = (WmTimestamp) e->time;
		break;
	}
	case XCB_ENTER_NOTIFY: {
		xcb_enter_notify_event_t *e  = (xcb_enter_notify_event_t *) xe;
		ev->type                     = AWM_EV_ENTER;
		ev->window                   = (WinId) e->event;
		ev->enter_leave_focus.mode   = e->mode;
		ev->enter_leave_focus.detail = e->detail;
		break;
	}
	case XCB_LEAVE_NOTIFY: {
		xcb_leave_notify_event_t *e  = (xcb_leave_notify_event_t *) xe;
		ev->type                     = AWM_EV_LEAVE;
		ev->window                   = (WinId) e->event;
		ev->enter_leave_focus.mode   = e->mode;
		ev->enter_leave_focus.detail = e->detail;
		break;
	}
	case XCB_FOCUS_IN: {
		xcb_focus_in_event_t *e      = (xcb_focus_in_event_t *) xe;
		ev->type                     = AWM_EV_FOCUS_IN;
		ev->window                   = (WinId) e->event;
		ev->enter_leave_focus.mode   = e->mode;
		ev->enter_leave_focus.detail = e->detail;
		break;
	}
	case XCB_EXPOSE: {
		xcb_expose_event_t *e = (xcb_expose_event_t *) xe;
		ev->type              = AWM_EV_EXPOSE;
		ev->window            = (WinId) e->window;
		ev->expose.count      = e->count;
		break;
	}
	case XCB_MOTION_NOTIFY: {
		xcb_motion_notify_event_t *e = (xcb_motion_notify_event_t *) xe;
		ev->type                     = AWM_EV_MOTION;
		ev->window                   = (WinId) e->event;
		ev->motion.time              = (WmTimestamp) e->time;
		ev->motion.root_x            = e->root_x;
		ev->motion.root_y            = e->root_y;
		ev->motion.detail            = e->detail;
		break;
	}
	case XCB_CONFIGURE_NOTIFY: {
		xcb_configure_notify_event_t *e = (xcb_configure_notify_event_t *) xe;
		ev->type                        = AWM_EV_CONFIGURE_NOTIFY;
		ev->window                      = (WinId) e->window;
		ev->configure.x                 = e->x;
		ev->configure.y                 = e->y;
		ev->configure.w                 = e->width;
		ev->configure.h                 = e->height;
		ev->configure.bw                = e->border_width;
		ev->configure.above             = (WinId) e->above_sibling;
		ev->configure.is_send_event     = (xe->response_type & 0x80) ? 1 : 0;
		break;
	}
	case XCB_CONFIGURE_REQUEST: {
		xcb_configure_request_event_t *e =
		    (xcb_configure_request_event_t *) xe;
		ev->type                     = AWM_EV_CONFIGURE_REQUEST;
		ev->window                   = (WinId) e->window;
		ev->configure_req.value_mask = e->value_mask;
		ev->configure_req.x          = e->x;
		ev->configure_req.y          = e->y;
		ev->configure_req.w          = e->width;
		ev->configure_req.h          = e->height;
		ev->configure_req.bw         = e->border_width;
		ev->configure_req.sibling    = (WinId) e->sibling;
		ev->configure_req.stack_mode = e->stack_mode;
		break;
	}
	case XCB_MAP_REQUEST: {
		xcb_map_request_event_t *e = (xcb_map_request_event_t *) xe;
		ev->type                   = AWM_EV_MAP_REQUEST;
		ev->window                 = (WinId) e->window;
		ev->map_request.parent     = (WinId) e->parent;
		break;
	}
	case XCB_UNMAP_NOTIFY: {
		xcb_unmap_notify_event_t *e     = (xcb_unmap_notify_event_t *) xe;
		ev->type                        = AWM_EV_UNMAP_NOTIFY;
		ev->window                      = (WinId) e->window;
		ev->unmap_destroy.event         = (WinId) e->event;
		ev->unmap_destroy.is_send_event = (xe->response_type & 0x80) ? 1 : 0;
		break;
	}
	case XCB_DESTROY_NOTIFY: {
		xcb_destroy_notify_event_t *e   = (xcb_destroy_notify_event_t *) xe;
		ev->type                        = AWM_EV_DESTROY_NOTIFY;
		ev->window                      = (WinId) e->window;
		ev->unmap_destroy.event         = (WinId) e->event;
		ev->unmap_destroy.is_send_event = 0;
		break;
	}
	case XCB_PROPERTY_NOTIFY: {
		xcb_property_notify_event_t *e = (xcb_property_notify_event_t *) xe;
		ev->type                       = AWM_EV_PROPERTY_NOTIFY;
		ev->window                     = (WinId) e->window;
		ev->property.atom              = (AtomId) e->atom;
		ev->property.state             = e->state;
		break;
	}
	case XCB_CLIENT_MESSAGE: {
		xcb_client_message_event_t *e = (xcb_client_message_event_t *) xe;
		ev->type                      = AWM_EV_CLIENT_MESSAGE;
		ev->window                    = (WinId) e->window;
		ev->client_message.msg_type   = (AtomId) e->type;
		ev->client_message.data[0]    = e->data.data32[0];
		ev->client_message.data[1]    = e->data.data32[1];
		ev->client_message.data[2]    = e->data.data32[2];
		ev->client_message.data[3]    = e->data.data32[3];
		ev->client_message.data[4]    = e->data.data32[4];
		break;
	}
	case XCB_RESIZE_REQUEST: {
		xcb_resize_request_event_t *e = (xcb_resize_request_event_t *) xe;
		ev->type                      = AWM_EV_RESIZE_REQUEST;
		ev->window                    = (WinId) e->window;
		ev->resize_request.w          = e->width;
		ev->resize_request.h          = e->height;
		break;
	}
	case XCB_MAPPING_NOTIFY: {
		xcb_mapping_notify_event_t *e = (xcb_mapping_notify_event_t *) xe;
		ev->type                      = AWM_EV_MAPPING_NOTIFY;
		ev->window                    = WIN_NONE;
		ev->mapping.request           = e->request;
		ev->mapping.first_keycode     = e->first_keycode;
		ev->mapping.count             = e->count;
		break;
	}
	default:
		return 0;
	}
	return 1;
}

static AwmEvent *
x11_poll_event(PlatformCtx *p)
{
	xcb_generic_event_t *xe = xcb_poll_for_event(p->xc);
	AwmEvent            *ev;

	if (!xe)
		return NULL;
	ev = malloc(sizeof *ev);
	if (!ev) {
		free(xe);
		return NULL;
	}
	if (!awm_event_from_xcb(xe, ev)) {
		/* Unknown type: store as NONE but keep the raw type in window
		 * field so awm.c can still index handler[] via the xcb type. */
		ev->type   = AWM_EV_NONE;
		ev->window = (WinId) (uintptr_t) xe; /* carry raw ptr through */
	}
	/* Note: xcb event is kept alive; awm.c dispatch must free *both*
	 * the AwmEvent and the original xcb event.  For now we embed the
	 * xcb pointer in window when type==AWM_EV_NONE so awm.c can route
	 * it.  Proper full dispatch migration happens in Step 4. */
	free(xe);
	return ev;
}

static AwmEvent *
x11_next_event(PlatformCtx *p)
{
	xcb_generic_event_t *xe = xcb_wait_for_event(p->xc);
	AwmEvent            *ev;

	if (!xe)
		return NULL;
	ev = malloc(sizeof *ev);
	if (!ev) {
		free(xe);
		return NULL;
	}
	if (!awm_event_from_xcb(xe, ev)) {
		ev->type   = AWM_EV_NONE;
		ev->window = WIN_NONE;
	}
	free(xe);
	return ev;
}

/* -------------------------------------------------------------------------
 * keyboard grab
 * ---------------------------------------------------------------------- */

static void
x11_grab_keyboard(PlatformCtx *p, WinId w, WmTimestamp t)
{
	xcb_grab_keyboard(p->xc, 0, (xcb_window_t) w, (xcb_timestamp_t) t,
	    XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC);
}

static void
x11_ungrab_keyboard(PlatformCtx *p, WmTimestamp t)
{
	xcb_ungrab_keyboard(p->xc, (xcb_timestamp_t) t);
}

static void
x11_ungrab_key(PlatformCtx *p, WinId w)
{
	xcb_ungrab_key(p->xc, XCB_GRAB_ANY, (xcb_window_t) w, XCB_MOD_MASK_ANY);
}

/* -------------------------------------------------------------------------
 * root window tree
 * ---------------------------------------------------------------------- */

static int
x11_query_root_tree(PlatformCtx *p, WinId **wins_out, int *n_out)
{
	xcb_query_tree_cookie_t ck = xcb_query_tree(p->xc, p->root);
	xcb_query_tree_reply_t *tr = xcb_query_tree_reply(p->xc, ck, NULL);

	if (!tr)
		return 0;
	{
		int           n    = xcb_query_tree_children_length(tr);
		xcb_window_t *src  = xcb_query_tree_children(tr);
		WinId        *copy = NULL;

		if (n > 0) {
			int i;
			copy = malloc((size_t) n * sizeof(WinId));
			if (!copy) {
				free(tr);
				return 0;
			}
			for (i = 0; i < n; i++)
				copy[i] = (WinId) src[i];
		}
		free(tr);
		*wins_out = copy;
		*n_out    = n;
	}
	return 1;
}

/* -------------------------------------------------------------------------
 * window attributes batch
 * ---------------------------------------------------------------------- */

static int
x11_get_window_attributes(
    PlatformCtx *p, WinId w, int *override_redirect_out, int *map_state_out)
{
	xcb_get_window_attributes_cookie_t ck =
	    xcb_get_window_attributes(p->xc, (xcb_window_t) w);
	xcb_get_window_attributes_reply_t *r =
	    xcb_get_window_attributes_reply(p->xc, ck, NULL);

	if (!r)
		return 0;
	*override_redirect_out = r->override_redirect;
	*map_state_out         = r->map_state;
	free(r);
	return 1;
}

/* -------------------------------------------------------------------------
 * raw property read
 * ---------------------------------------------------------------------- */

static xcb_get_property_reply_t *
x11_get_prop_raw(
    PlatformCtx *p, WinId w, AtomId prop, AtomId type, uint32_t long_length)
{
	xcb_get_property_cookie_t ck = xcb_get_property(p->xc, 0, (xcb_window_t) w,
	    (xcb_atom_t) prop, (xcb_atom_t) type, 0, long_length);
	return xcb_get_property_reply(p->xc, ck, NULL);
}

/* -------------------------------------------------------------------------
 * connection introspection
 * ---------------------------------------------------------------------- */

static int
x11_get_connection_fd(PlatformCtx *p)
{
	if (!p->xc)
		return -1;
	return xcb_get_file_descriptor(p->xc);
}

/* -------------------------------------------------------------------------
 * synchronous round-trip
 * ---------------------------------------------------------------------- */

static void
x11_sync(PlatformCtx *p)
{
	xcb_get_input_focus_cookie_t ck = xcb_get_input_focus(p->xc);
	xcb_get_input_focus_reply_t *r =
	    xcb_get_input_focus_reply(p->xc, ck, NULL);

	free(r);
}

/* -------------------------------------------------------------------------
 * pixmap / colormap teardown
 * ---------------------------------------------------------------------- */

static void
x11_free_pixmap(PlatformCtx *p, WmPixmap pm)
{
	xcb_free_pixmap(p->xc, (xcb_pixmap_t) pm);
}

static void
x11_free_colormap(PlatformCtx *p, WmColormap cm)
{
	xcb_free_colormap(p->xc, (xcb_colormap_t) cm);
}

/* -------------------------------------------------------------------------
 * atom interning
 * ---------------------------------------------------------------------- */

static void
x11_intern_atoms_batch(
    PlatformCtx *p, const char **names, AtomId *atoms_out, int n)
{
	xcb_intern_atom_cookie_t *cookies;
	xcb_intern_atom_reply_t  *reply;
	int                       i;

	if (n <= 0)
		return;
	cookies = ecalloc((size_t) n, sizeof *cookies);
	for (i = 0; i < n; i++) {
		uint16_t nlen = (uint16_t) strlen(names[i]);
		cookies[i]    = xcb_intern_atom(p->xc, 0, nlen, names[i]);
	}
	for (i = 0; i < n; i++) {
		reply        = xcb_intern_atom_reply(p->xc, cookies[i], NULL);
		atoms_out[i] = reply ? reply->atom : XCB_ATOM_NONE;
		free(reply);
	}
	free(cookies);
}

/* -------------------------------------------------------------------------
 * generic window creation
 * ---------------------------------------------------------------------- */

static WinId
x11_create_window(PlatformCtx *p, WinId parent, int x, int y, int w, int h,
    uint32_t val_mask, const uint32_t *vals)
{
	xcb_window_t wid = xcb_generate_id(p->xc);

	xcb_create_window(p->xc, XCB_COPY_FROM_PARENT, wid, (xcb_window_t) parent,
	    (int16_t) x, (int16_t) y, (uint16_t) w, (uint16_t) h, 0,
	    XCB_WINDOW_CLASS_INPUT_OUTPUT, XCB_COPY_FROM_PARENT, val_mask, vals);
	return (WinId) wid;
}

/* -------------------------------------------------------------------------
 * root event mask / cursor selection
 * ---------------------------------------------------------------------- */

static void
x11_select_root_events(PlatformCtx *p, uint32_t mask)
{
	xcb_change_window_attributes(p->xc, p->root, XCB_CW_EVENT_MASK, &mask);
}

static void
x11_set_root_cursor(PlatformCtx *p, WmCursor cursor)
{
	uint32_t val = (uint32_t) cursor;
	xcb_change_window_attributes(p->xc, p->root, XCB_CW_CURSOR, &val);
}

/* -------------------------------------------------------------------------
 * RandR setup
 * ---------------------------------------------------------------------- */

#ifdef XRANDR
static void
x11_randr_init(PlatformCtx *p)
{
	const xcb_query_extension_reply_t *ext =
	    xcb_get_extension_data(p->xc, &xcb_randr_id);

	if (ext && ext->present) {
		p->randrbase = ext->first_event;
		p->rrerrbase = ext->first_error;
		xcb_randr_select_input(
		    p->xc, p->root, XCB_RANDR_NOTIFY_MASK_SCREEN_CHANGE);
	}
}

static double
x11_randr_probe_dpi(PlatformCtx *p)
{
	xcb_randr_get_screen_resources_current_cookie_t src;
	xcb_randr_get_screen_resources_current_reply_t *sr;
	xcb_randr_crtc_t                               *crtcs;
	int                                             i;
	double                                          dpi = 0.0;

	src = xcb_randr_get_screen_resources_current(p->xc, p->root);
	sr  = xcb_randr_get_screen_resources_current_reply(p->xc, src, NULL);
	if (!sr)
		return 0.0;
	crtcs = xcb_randr_get_screen_resources_current_crtcs(sr);
	for (i = 0; i < (int) sr->num_crtcs && dpi == 0.0; i++) {
		xcb_randr_get_crtc_info_cookie_t cic;
		xcb_randr_get_crtc_info_reply_t *ci;
		xcb_randr_output_t              *outputs;

		cic = xcb_randr_get_crtc_info(p->xc, crtcs[i], XCB_CURRENT_TIME);
		ci  = xcb_randr_get_crtc_info_reply(p->xc, cic, NULL);
		if (!ci || ci->num_outputs == 0 || ci->width == 0) {
			free(ci);
			continue;
		}
		outputs = xcb_randr_get_crtc_info_outputs(ci);
		{
			xcb_randr_get_output_info_cookie_t oic;
			xcb_randr_get_output_info_reply_t *oi;

			oic =
			    xcb_randr_get_output_info(p->xc, outputs[0], XCB_CURRENT_TIME);
			oi = xcb_randr_get_output_info_reply(p->xc, oic, NULL);
			if (oi && oi->mm_width > 0)
				dpi = (double) ci->width / ((double) oi->mm_width / 25.4);
			free(oi);
		}
		free(ci);
	}
	free(sr);
	return dpi;
}
#endif /* XRANDR */

/* -------------------------------------------------------------------------
 * screen setup
 * ---------------------------------------------------------------------- */

static void
x11_init_screen(PlatformCtx *p)
{
	xcb_screen_iterator_t sit = xcb_setup_roots_iterator(xcb_get_setup(p->xc));
	int                   i;

	for (i = 0; i < p->screen; i++)
		xcb_screen_next(&sit);
	p->sw   = (int) sit.data->width_in_pixels;
	p->sh   = (int) sit.data->height_in_pixels;
	p->root = sit.data->root;
}

/* -------------------------------------------------------------------------
 * key symbol table lifecycle
 * ---------------------------------------------------------------------- */

static void
x11_keysyms_alloc(PlatformCtx *p)
{
	p->keysyms = xcb_key_symbols_alloc(p->xc);
}

static void
x11_keysyms_free(PlatformCtx *p)
{
	if (p->keysyms) {
		xcb_key_symbols_free(p->keysyms);
		p->keysyms = NULL;
	}
}

/* -------------------------------------------------------------------------
 * resource manager
 * ---------------------------------------------------------------------- */

static char *
x11_get_resource_manager(PlatformCtx *p)
{
	xcb_intern_atom_cookie_t  ack;
	xcb_intern_atom_reply_t  *ar;
	xcb_atom_t                res_mgr;
	xcb_get_property_cookie_t pck;
	xcb_get_property_reply_t *pr;
	char                     *resm;

	ack = xcb_intern_atom(
	    p->xc, 1, strlen("RESOURCE_MANAGER"), "RESOURCE_MANAGER");
	ar = xcb_intern_atom_reply(p->xc, ack, NULL);
	if (!ar || ar->atom == XCB_ATOM_NONE) {
		free(ar);
		return NULL;
	}
	res_mgr = ar->atom;
	free(ar);

	pck = xcb_get_property(
	    p->xc, 0, p->root, res_mgr, XCB_ATOM_STRING, 0, 65536);
	pr = xcb_get_property_reply(p->xc, pck, NULL);
	if (!pr || pr->type == XCB_ATOM_NONE || pr->format != 8 ||
	    pr->value_len == 0) {
		free(pr);
		return NULL;
	}
	resm = malloc(pr->value_len + 1);
	if (!resm) {
		free(pr);
		return NULL;
	}
	memcpy(resm, xcb_get_property_value(pr), pr->value_len);
	resm[pr->value_len] = '\0';
	free(pr);
	return resm;
}

/* -------------------------------------------------------------------------
 * XCB async error handler
 * ---------------------------------------------------------------------- */

/* Return a human-readable string for a base X11 error code (1-17).
 * Extension errors (codes > 127) are labelled generically. */
static const char *
xcb_error_text(uint8_t error_code)
{
	/* X11 core error codes — xproto.h §XCB_REQUEST … XCB_IMPLEMENTATION */
	static const char *names[] = {
		/* 0 */ "Success",
		/* 1 */ "BadRequest",
		/* 2 */ "BadValue",
		/* 3 */ "BadWindow",
		/* 4 */ "BadPixmap",
		/* 5 */ "BadAtom",
		/* 6 */ "BadCursor",
		/* 7 */ "BadFont",
		/* 8 */ "BadMatch",
		/* 9 */ "BadDrawable",
		/* 10 */ "BadAccess",
		/* 11 */ "BadAlloc",
		/* 12 */ "BadColor",
		/* 13 */ "BadGC",
		/* 14 */ "BadIDChoice",
		/* 15 */ "BadName",
		/* 16 */ "BadLength",
		/* 17 */ "BadImplementation",
	};
	if (error_code < (sizeof names / sizeof names[0]))
		return names[error_code];
	return "ExtensionError";
}

/* XCB async error handler — called from x_dispatch_cb() when the event
 * response_type is 0 (error packet).  Mirrors the old Xlib xerror() logic
 * but operates entirely on xcb_generic_error_t fields.
 *
 * Return values:  0 = benign, silently ignored.
 *                 1 = unexpected; logged via awm_error but execution
 * continues.
 *
 * Unlike the old Xlib handler we do NOT call exit() on unexpected errors —
 * the async nature of XCB means some races are unavoidable and a WM must
 * survive them.  Truly fatal conditions (X server death) are caught via the
 * HUP/ERR path in platform_source_dispatch(). */
int
xcb_error_handler(xcb_generic_error_t *e)
{
	uint8_t req = e->major_code;
	uint8_t err = e->error_code;

	/* Whitelist benign async errors that arise routinely in a WM:
	 * - BadWindow:   window destroyed between our request and the reply
	 * - SetInputFocus + BadMatch:   window became unviewable/unmapped
	 * - PolyText8/PolyFillRectangle/PolySegment/CopyArea + BadDrawable:
	 *     drawable destroyed while we were drawing
	 * - ConfigureWindow + BadMatch: sibling ordering race
	 * - GrabButton/GrabKey + BadAccess: another client owns the grab */
	if (err == XCB_WINDOW || (req == X_SetInputFocus && err == XCB_MATCH) ||
	    (req == X_PolyText8 && err == XCB_DRAWABLE) ||
	    (req == X_PolyFillRectangle && err == XCB_DRAWABLE) ||
	    (req == X_PolySegment && err == XCB_DRAWABLE) ||
	    (req == X_ConfigureWindow && err == XCB_MATCH) ||
	    (req == X_GrabButton && err == XCB_ACCESS) ||
	    (req == X_GrabKey && err == XCB_ACCESS) ||
	    (req == X_CopyArea && err == XCB_DRAWABLE))
		return 0;
#ifdef COMPOSITOR
	/* Transient XRender errors (BadPicture, BadPictFormat) arise when a GL
	 * window exits while a compositor repaint is in flight. */
	{
		int render_req, render_err;
		compositor_xrender_errors(&render_req, &render_err);
		if (render_req > 0 && req == (uint8_t) render_req &&
		    (err == (uint8_t) render_err             /* BadPicture    */
		        || err == (uint8_t) (render_err + 1) /* BadPictFormat */
		        || err == XCB_DRAWABLE || err == XCB_PIXMAP))
			return 0;
	}
	/* Transient XDamage errors (BadDamage) when a window is destroyed
	 * while we call xcb_damage_destroy on its damage handle.
	 * BadIDChoice on XDamage Subtract arises when a stale DAMAGE_NOTIFY
	 * event fires after comp_free_win() already destroyed the damage
	 * object — the event was queued before the destroy, so we still try
	 * to ack it and get an async BadIDChoice.  Both are benign. */
	{
		int damage_req, damage_err;
		compositor_damage_errors(&damage_req, &damage_err);
		if (damage_err > 0 && err == (uint8_t) damage_err)
			return 0;
		if (damage_req > 0 && req == (uint8_t) damage_req &&
		    err == XCB_ID_CHOICE)
			return 0;
	}
	/* Transient X Present errors (BadIDChoice) arise when a stale
	 * PresentCompleteNotify or similar event fires after comp_free_win()
	 * already destroyed the Present event subscription (EID).  The X
	 * server sends BadIDChoice on the next xcb_present_select_input
	 * referencing that EID.  Benign — ignore. */
	{
		int present_req;
		compositor_present_errors(&present_req);
		if (present_req > 0 && req == (uint8_t) present_req &&
		    err == XCB_ID_CHOICE)
			return 0;
	}
	/* GLX errors are stubs — compositor_glx_errors always returns -1 */
	{
		int glx_req, glx_err;
		compositor_glx_errors(&glx_req, &glx_err);
		(void) glx_req;
		(void) glx_err;
	}
#endif
	awm_error("X11 async error: %s (major=%d minor=%d error=%d resource=0x%x)",
	    xcb_error_text(err), (int) req, (int) e->minor_code, (int) err,
	    (unsigned) e->resource_id);
	return 1;
}

/* -------------------------------------------------------------------------
 * singleton instances
 * ---------------------------------------------------------------------- */

PlatformCtx g_plat;

WmBackend wm_backend_x11 = {
	.flush                    = x11_flush,
	.configure_win            = x11_configure_win,
	.change_attr              = x11_change_attr,
	.map                      = x11_map,
	.unmap                    = x11_unmap,
	.destroy_win              = x11_destroy_win,
	.send_configure_notify    = x11_send_configure_notify,
	.set_input_focus          = x11_set_input_focus,
	.warp_pointer             = x11_warp_pointer,
	.allow_events             = x11_allow_events,
	.grab_pointer             = x11_grab_pointer,
	.ungrab_pointer           = x11_ungrab_pointer,
	.ungrab_button            = x11_ungrab_button,
	.query_pointer            = x11_query_pointer,
	.grab_server              = x11_grab_server,
	.ungrab_server            = x11_ungrab_server,
	.get_override_redirect    = x11_get_override_redirect,
	.get_event_mask           = x11_get_event_mask,
	.get_geometry             = x11_get_geometry,
	.is_window_descendant     = x11_is_window_descendant,
	.get_atom_prop            = x11_get_atom_prop,
	.get_text_prop            = x11_get_text_prop,
	.get_wm_icon              = x11_get_wm_icon,
	.get_wm_class             = x11_get_wm_class,
	.change_prop              = x11_change_prop,
	.delete_prop              = x11_delete_prop,
	.kill_client_hard         = x11_kill_client_hard,
	.change_save_set          = x11_change_save_set,
	.reparent_window          = x11_reparent_window,
	.create_bar_win           = x11_create_bar_win,
	.screen_depth             = x11_screen_depth,
	.get_wm_normal_hints      = x11_get_wm_normal_hints,
	.get_wm_hints             = x11_get_wm_hints,
	.set_wm_hints             = x11_set_wm_hints,
	.get_wm_transient_for     = x11_get_wm_transient_for,
	.grab_buttons             = x11_grab_buttons,
	.update_numlock_mask      = x11_update_numlock_mask,
	.grab_keys_full           = x11_grab_keys_full,
	.refresh_keyboard_mapping = x11_refresh_keyboard_mapping,
	.get_keysym               = x11_get_keysym,
	.check_other_wm           = x11_check_other_wm,
	.update_geom              = x11_update_geom,
	.poll_event               = x11_poll_event,
	.next_event               = x11_next_event,
	.grab_keyboard            = x11_grab_keyboard,
	.ungrab_keyboard          = x11_ungrab_keyboard,
	.ungrab_key               = x11_ungrab_key,
	.query_root_tree          = x11_query_root_tree,
	.get_window_attributes    = x11_get_window_attributes,
	.get_prop_raw             = x11_get_prop_raw,
	.get_connection_fd        = x11_get_connection_fd,
	.sync                     = x11_sync,
	.free_pixmap              = x11_free_pixmap,
	.free_colormap            = x11_free_colormap,
	.intern_atoms_batch       = x11_intern_atoms_batch,
	.create_window            = x11_create_window,
	.select_root_events       = x11_select_root_events,
	.set_root_cursor          = x11_set_root_cursor,
#ifdef XRANDR
	.randr_init      = x11_randr_init,
	.randr_probe_dpi = x11_randr_probe_dpi,
#endif
	.init_screen          = x11_init_screen,
	.keysyms_alloc        = x11_keysyms_alloc,
	.keysyms_free         = x11_keysyms_free,
	.get_resource_manager = x11_get_resource_manager,
};

WmBackend *g_wm_backend = NULL;
