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
#include "events.h"
#include "monitor.h"
#include "spawn.h"
#include "switcher.h"
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
x11_configure_win(
    PlatformCtx *p, xcb_window_t w, uint16_t mask, const uint32_t *vals)
{
	xcb_configure_window(p->xc, w, mask, vals);
}

/* -------------------------------------------------------------------------
 * window attributes
 * ---------------------------------------------------------------------- */

static void
x11_change_attr(
    PlatformCtx *p, xcb_window_t w, uint32_t mask, const uint32_t *vals)
{
	xcb_change_window_attributes(p->xc, w, mask, vals);
}

/* -------------------------------------------------------------------------
 * window visibility
 * ---------------------------------------------------------------------- */

static void
x11_map(PlatformCtx *p, xcb_window_t w)
{
	xcb_map_window(p->xc, w);
}

static void
x11_unmap(PlatformCtx *p, xcb_window_t w)
{
	xcb_unmap_window(p->xc, w);
}

static void
x11_destroy_win(PlatformCtx *p, xcb_window_t w)
{
	xcb_destroy_window(p->xc, w);
}

/* -------------------------------------------------------------------------
 * window events
 * ---------------------------------------------------------------------- */

static void
x11_send_configure_notify(
    PlatformCtx *p, xcb_window_t w, int x, int y, int bw, int ww, int wh)
{
	xcb_configure_notify_event_t ce;

	ce.response_type     = XCB_CONFIGURE_NOTIFY;
	ce.pad0              = 0;
	ce.sequence          = 0;
	ce.event             = w;
	ce.window            = w;
	ce.above_sibling     = XCB_NONE;
	ce.x                 = (int16_t) x;
	ce.y                 = (int16_t) y;
	ce.width             = (uint16_t) ww;
	ce.height            = (uint16_t) wh;
	ce.border_width      = (uint16_t) bw;
	ce.override_redirect = 0;
	ce.pad1              = 0;
	xcb_send_event(
	    p->xc, 0, w, XCB_EVENT_MASK_STRUCTURE_NOTIFY, (const char *) &ce);
}

/* -------------------------------------------------------------------------
 * focus & input
 * ---------------------------------------------------------------------- */

static void
x11_set_input_focus(PlatformCtx *p, xcb_window_t w, xcb_timestamp_t t)
{
	xcb_set_input_focus(p->xc, XCB_INPUT_FOCUS_POINTER_ROOT, w, t);
}

static void
x11_warp_pointer(PlatformCtx *p, xcb_window_t dst, int16_t x, int16_t y)
{
	xcb_warp_pointer(p->xc, XCB_WINDOW_NONE, dst, 0, 0, 0, 0, x, y);
}

static void
x11_allow_events(PlatformCtx *p, int mode, xcb_timestamp_t t)
{
	xcb_allow_events(p->xc, (uint8_t) mode, t);
}

/* -------------------------------------------------------------------------
 * pointer grab
 * ---------------------------------------------------------------------- */

static int
x11_grab_pointer(PlatformCtx *p, xcb_cursor_t cur)
{
	xcb_grab_pointer_cookie_t ck =
	    xcb_grab_pointer(p->xc, 0, p->root, MOUSEMASK, XCB_GRAB_MODE_ASYNC,
	        XCB_GRAB_MODE_ASYNC, XCB_WINDOW_NONE, cur, XCB_CURRENT_TIME);
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
x11_ungrab_button(PlatformCtx *p, xcb_window_t w)
{
	xcb_ungrab_button(p->xc, XCB_BUTTON_INDEX_ANY, w, XCB_MOD_MASK_ANY);
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
x11_get_override_redirect(PlatformCtx *p, xcb_window_t w, int *out)
{
	xcb_get_window_attributes_cookie_t ck =
	    xcb_get_window_attributes(p->xc, w);
	xcb_get_window_attributes_reply_t *r =
	    xcb_get_window_attributes_reply(p->xc, ck, NULL);

	if (!r)
		return 0;
	*out = r->override_redirect;
	free(r);
	return 1;
}

static int
x11_get_event_mask(PlatformCtx *p, xcb_window_t w, uint32_t *out)
{
	xcb_get_window_attributes_cookie_t ck =
	    xcb_get_window_attributes(p->xc, w);
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
    PlatformCtx *p, xcb_window_t w, int *x, int *y, int *ww, int *wh, int *bw)
{
	xcb_get_geometry_cookie_t ck = xcb_get_geometry(p->xc, w);
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
x11_is_window_descendant(PlatformCtx *p, xcb_window_t w, xcb_window_t ancestor)
{
	while (w && w != ancestor && w != p->root) {
		xcb_query_tree_reply_t *r =
		    xcb_query_tree_reply(p->xc, xcb_query_tree(p->xc, w), NULL);
		if (!r)
			break;
		w = r->parent;
		free(r);
	}
	return w == ancestor;
}

/* -------------------------------------------------------------------------
 * property reads
 * ---------------------------------------------------------------------- */

static xcb_atom_t
x11_get_atom_prop(
    PlatformCtx *p, xcb_window_t w, xcb_atom_t prop, xcb_atom_t type)
{
	xcb_get_property_cookie_t ck =
	    xcb_get_property(p->xc, 0, w, prop, type, 0, 1);
	xcb_get_property_reply_t *r    = xcb_get_property_reply(p->xc, ck, NULL);
	xcb_atom_t                atom = XCB_ATOM_NONE;

	if (r && xcb_get_property_value_length(r) >= (int) sizeof(xcb_atom_t))
		atom = *(xcb_atom_t *) xcb_get_property_value(r);
	free(r);
	return atom;
}

static int
x11_get_text_prop(PlatformCtx *p, xcb_window_t w, xcb_atom_t atom, char *buf,
    unsigned int size)
{
	xcb_get_property_cookie_t           ck;
	xcb_icccm_get_text_property_reply_t prop;
	unsigned int                        len;

	if (!buf || size == 0)
		return 0;
	buf[0] = '\0';
	ck     = xcb_icccm_get_text_property(p->xc, w, atom);
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
x11_get_wm_icon(
    PlatformCtx *p, xcb_window_t w, uint32_t **data_out, int *nitems_out)
{
	xcb_get_property_cookie_t ck = xcb_get_property(
	    p->xc, 0, w, p->netatom[NetWMIcon], XCB_ATOM_ANY, 0, UINT32_MAX / 4);
	xcb_get_property_reply_t *r = xcb_get_property_reply(p->xc, ck, NULL);
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
x11_get_wm_class(PlatformCtx *p, xcb_window_t w, char *inst_buf,
    unsigned int inst_size, char *cls_buf, unsigned int cls_size)
{
	xcb_get_property_cookie_t ck = xcb_get_property(
	    p->xc, 0, w, XCB_ATOM_WM_CLASS, XCB_ATOM_STRING, 0, 512);
	xcb_get_property_reply_t *r = xcb_get_property_reply(p->xc, ck, NULL);

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
x11_change_prop(PlatformCtx *p, xcb_window_t w, xcb_atom_t prop,
    xcb_atom_t type, int format, uint8_t mode, int n, const void *data)
{
	xcb_change_property(
	    p->xc, mode, w, prop, type, (uint8_t) format, (uint32_t) n, data);
}

static void
x11_delete_prop(PlatformCtx *p, xcb_window_t w, xcb_atom_t prop)
{
	xcb_delete_property(p->xc, w, prop);
}

/* -------------------------------------------------------------------------
 * kill / close
 * ---------------------------------------------------------------------- */

static void
x11_kill_client_hard(PlatformCtx *p, xcb_window_t w)
{
	xcb_grab_server(p->xc);
	xcb_set_close_down_mode(p->xc, XCB_CLOSE_DOWN_DESTROY_ALL);
	xcb_kill_client(p->xc, w);
	xcb_ungrab_server(p->xc);
}

/* -------------------------------------------------------------------------
 * save-set / reparent (systray)
 * ---------------------------------------------------------------------- */

static void
x11_change_save_set(PlatformCtx *p, xcb_window_t w, int insert)
{
	xcb_change_save_set(
	    p->xc, insert ? XCB_SET_MODE_INSERT : XCB_SET_MODE_DELETE, w);
}

static void
x11_reparent_window(
    PlatformCtx *p, xcb_window_t w, xcb_window_t parent, int x, int y)
{
	xcb_reparent_window(p->xc, w, parent, (int16_t) x, (int16_t) y);
}

/* -------------------------------------------------------------------------
 * bar window creation
 * ---------------------------------------------------------------------- */

static xcb_window_t
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
x11_get_wm_normal_hints(PlatformCtx *p, xcb_window_t w, xcb_size_hints_t *out)
{
	xcb_get_property_cookie_t ck = xcb_icccm_get_wm_normal_hints(p->xc, w);

	return xcb_icccm_get_wm_normal_hints_reply(p->xc, ck, out, NULL);
}

static int
x11_get_wm_hints(PlatformCtx *p, xcb_window_t w, xcb_icccm_wm_hints_t *out)
{
	xcb_get_property_cookie_t ck = xcb_icccm_get_wm_hints(p->xc, w);

	return xcb_icccm_get_wm_hints_reply(p->xc, ck, out, NULL);
}

static void
x11_set_wm_hints(
    PlatformCtx *p, xcb_window_t w, const xcb_icccm_wm_hints_t *hints)
{
	/* xcb_icccm_set_wm_hints takes a non-const pointer; cast is safe since
	 * the function does not modify the hints struct. */
	xcb_icccm_set_wm_hints(p->xc, w, (xcb_icccm_wm_hints_t *) hints);
}

static xcb_window_t
x11_get_wm_transient_for(PlatformCtx *p, xcb_window_t w)
{
	xcb_window_t trans = XCB_WINDOW_NONE;

	xcb_icccm_get_wm_transient_for_reply(
	    p->xc, xcb_icccm_get_wm_transient_for(p->xc, w), &trans, NULL);
	return trans;
}

/* -------------------------------------------------------------------------
 * compound / higher-level operations
 * ---------------------------------------------------------------------- */

static void
x11_grab_buttons(PlatformCtx *p, xcb_window_t w, int focused)
{
	unsigned int i, j;
	unsigned int modifiers[] = { 0, XCB_MOD_MASK_LOCK, p->numlockmask,
		p->numlockmask | XCB_MOD_MASK_LOCK };

	xcb_ungrab_button(p->xc, XCB_BUTTON_INDEX_ANY, w, XCB_MOD_MASK_ANY);
	if (!focused)
		xcb_grab_button(p->xc, 0, w,
		    XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_BUTTON_RELEASE,
		    XCB_GRAB_MODE_SYNC, XCB_GRAB_MODE_SYNC, XCB_WINDOW_NONE,
		    XCB_CURSOR_NONE, XCB_BUTTON_INDEX_ANY, XCB_MOD_MASK_ANY);
	for (i = 0; i < LENGTH(buttons); i++)
		if (buttons[i].click == ClkClientWin)
			for (j = 0; j < LENGTH(modifiers); j++)
				xcb_grab_button(p->xc, 0, w,
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
x11_refresh_keyboard_mapping(PlatformCtx *p, xcb_mapping_notify_event_t *ev)
{
	xcb_refresh_keyboard_mapping(p->keysyms, ev);
}

static xcb_keysym_t
x11_get_keysym(PlatformCtx *p, xcb_keycode_t code, int col)
{
	return xcb_key_symbols_get_keysym(p->keysyms, code, col);
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

static int
x11_update_geom(PlatformCtx *p)
{
	(void) p;
	return updategeom();
}

/* -------------------------------------------------------------------------
 * event loop helpers
 * ---------------------------------------------------------------------- */

static xcb_generic_event_t *
x11_poll_event(PlatformCtx *p)
{
	return xcb_poll_for_event(p->xc);
}

static xcb_generic_event_t *
x11_next_event(PlatformCtx *p)
{
	return xcb_wait_for_event(p->xc);
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
};

WmBackend *g_wm_backend = NULL;
