/* See LICENSE file for copyright and license details. */

#include "awm.h"

static void
stub_flush(PlatformCtx *p)
{
	(void) p;
}

static void
stub_configure_win(
    PlatformCtx *p, WinId w, uint16_t mask, const uint32_t *vals)
{
	(void) p;
	(void) w;
	(void) mask;
	(void) vals;
}

static void
stub_change_attr(PlatformCtx *p, WinId w, uint32_t mask, const uint32_t *vals)
{
	(void) p;
	(void) w;
	(void) mask;
	(void) vals;
}

static void
stub_map(PlatformCtx *p, WinId w)
{
	(void) p;
	(void) w;
}

static void
stub_unmap(PlatformCtx *p, WinId w)
{
	(void) p;
	(void) w;
}

static void
stub_destroy_win(PlatformCtx *p, WinId w)
{
	(void) p;
	(void) w;
}

static void
stub_send_configure_notify(
    PlatformCtx *p, WinId w, int x, int y, int bw, int ww, int wh)
{
	(void) p;
	(void) w;
	(void) x;
	(void) y;
	(void) bw;
	(void) ww;
	(void) wh;
}

static void
stub_set_input_focus(PlatformCtx *p, WinId w, WmTimestamp t)
{
	(void) p;
	(void) w;
	(void) t;
}

static void
stub_warp_pointer(PlatformCtx *p, WinId dst, int16_t x, int16_t y)
{
	(void) p;
	(void) dst;
	(void) x;
	(void) y;
}

static void
stub_allow_events(PlatformCtx *p, int mode, WmTimestamp t)
{
	(void) p;
	(void) mode;
	(void) t;
}

static int
stub_grab_pointer(PlatformCtx *p, WmCursor cursor)
{
	(void) p;
	(void) cursor;
	return 0;
}

static void
stub_ungrab_pointer(PlatformCtx *p)
{
	(void) p;
}

static void
stub_ungrab_button(PlatformCtx *p, WinId w)
{
	(void) p;
	(void) w;
}

static int
stub_query_pointer(PlatformCtx *p, int *x, int *y)
{
	(void) p;
	if (x)
		*x = 0;
	if (y)
		*y = 0;
	return 0;
}

static void
stub_grab_server(PlatformCtx *p)
{
	(void) p;
}

static void
stub_ungrab_server(PlatformCtx *p)
{
	(void) p;
}

static int
stub_get_override_redirect(PlatformCtx *p, WinId w, int *override_redirect_out)
{
	(void) p;
	(void) w;
	if (override_redirect_out)
		*override_redirect_out = 0;
	return 0;
}

static int
stub_get_event_mask(PlatformCtx *p, WinId w, uint32_t *event_mask_out)
{
	(void) p;
	(void) w;
	if (event_mask_out)
		*event_mask_out = 0;
	return 0;
}

static int
stub_get_geometry(
    PlatformCtx *p, WinId w, int *x, int *y, int *ww, int *wh, int *bw)
{
	(void) p;
	(void) w;
	if (x)
		*x = 0;
	if (y)
		*y = 0;
	if (ww)
		*ww = 0;
	if (wh)
		*wh = 0;
	if (bw)
		*bw = 0;
	return 0;
}

static int
stub_is_window_descendant(PlatformCtx *p, WinId w, WinId ancestor)
{
	(void) p;
	(void) w;
	(void) ancestor;
	return 0;
}

static AtomId
stub_get_atom_prop(PlatformCtx *p, WinId w, AtomId prop, AtomId type)
{
	(void) p;
	(void) w;
	(void) prop;
	(void) type;
	return ATOM_NONE;
}

static int
stub_get_text_prop(
    PlatformCtx *p, WinId w, AtomId atom, char *buf, unsigned int size)
{
	(void) p;
	(void) w;
	(void) atom;
	if (buf && size)
		buf[0] = '\0';
	return 0;
}

static int
stub_get_wm_icon(PlatformCtx *p, WinId w, uint32_t **data_out, int *nitems_out)
{
	(void) p;
	(void) w;
	if (data_out)
		*data_out = NULL;
	if (nitems_out)
		*nitems_out = 0;
	return 0;
}

static int
stub_get_wm_class(PlatformCtx *p, WinId w, char *inst_buf,
    unsigned int inst_size, char *cls_buf, unsigned int cls_size)
{
	(void) p;
	(void) w;
	if (inst_buf && inst_size)
		inst_buf[0] = '\0';
	if (cls_buf && cls_size)
		cls_buf[0] = '\0';
	return 0;
}

static void
stub_change_prop(PlatformCtx *p, WinId w, AtomId prop, AtomId type, int format,
    uint8_t mode, int n, const void *data)
{
	(void) p;
	(void) w;
	(void) prop;
	(void) type;
	(void) format;
	(void) mode;
	(void) n;
	(void) data;
}

static void
stub_delete_prop(PlatformCtx *p, WinId w, AtomId prop)
{
	(void) p;
	(void) w;
	(void) prop;
}

static void
stub_kill_client_hard(PlatformCtx *p, WinId w)
{
	(void) p;
	(void) w;
}

static void
stub_change_save_set(PlatformCtx *p, WinId w, int insert)
{
	(void) p;
	(void) w;
	(void) insert;
}

static void
stub_reparent_window(PlatformCtx *p, WinId w, WinId parent, int x, int y)
{
	(void) p;
	(void) w;
	(void) parent;
	(void) x;
	(void) y;
}

static WinId
stub_create_bar_win(
    PlatformCtx *p, int x, int y, int w, int h, int compositor_active)
{
	(void) p;
	(void) x;
	(void) y;
	(void) w;
	(void) h;
	(void) compositor_active;
	return WIN_NONE;
}

static int
stub_screen_depth(PlatformCtx *p)
{
	(void) p;
	return 0;
}

static int
stub_get_wm_normal_hints(PlatformCtx *p, WinId w, AwmSizeHints *out)
{
	(void) p;
	(void) w;
	if (out)
		out->flags = 0;
	return 0;
}

static int
stub_get_wm_hints(PlatformCtx *p, WinId w, AwmWmHints *out)
{
	(void) p;
	(void) w;
	if (out) {
		out->flags = 0;
		out->input = 1;
	}
	return 0;
}

static void
stub_set_wm_hints(PlatformCtx *p, WinId w, const AwmWmHints *hints)
{
	(void) p;
	(void) w;
	(void) hints;
}

static WinId
stub_get_wm_transient_for(PlatformCtx *p, WinId w)
{
	(void) p;
	(void) w;
	return WIN_NONE;
}

static void
stub_grab_buttons(PlatformCtx *p, WinId w, int focused)
{
	(void) p;
	(void) w;
	(void) focused;
}

static void
stub_update_numlock_mask(PlatformCtx *p)
{
	(void) p;
}

static void
stub_grab_keys_full(PlatformCtx *p)
{
	(void) p;
}

static void
stub_refresh_keyboard_mapping(PlatformCtx *p, AwmEvent *ev)
{
	(void) p;
	(void) ev;
}

static KeySym
stub_get_keysym(PlatformCtx *p, WmKeycode code, int col)
{
	(void) p;
	(void) code;
	(void) col;
	return 0;
}

static void
stub_check_other_wm(PlatformCtx *p)
{
	(void) p;
}

static int
stub_update_geom(PlatformCtx *p)
{
	(void) p;
	return 0;
}

static AwmEvent *
stub_poll_event(PlatformCtx *p)
{
	(void) p;
	return NULL;
}

static AwmEvent *
stub_next_event(PlatformCtx *p)
{
	(void) p;
	return NULL;
}

static void
stub_grab_keyboard(PlatformCtx *p, WinId w, WmTimestamp t)
{
	(void) p;
	(void) w;
	(void) t;
}

static void
stub_ungrab_keyboard(PlatformCtx *p, WmTimestamp t)
{
	(void) p;
	(void) t;
}

static void
stub_ungrab_key(PlatformCtx *p, WinId w)
{
	(void) p;
	(void) w;
}

static int
stub_query_root_tree(PlatformCtx *p, WinId **wins_out, int *n_out)
{
	(void) p;
	if (wins_out)
		*wins_out = NULL;
	if (n_out)
		*n_out = 0;
	return 0;
}

static int
stub_get_window_attributes(
    PlatformCtx *p, WinId w, int *override_redirect_out, int *map_state_out)
{
	(void) p;
	(void) w;
	if (override_redirect_out)
		*override_redirect_out = 0;
	if (map_state_out)
		*map_state_out = 0;
	return 0;
}

static int
stub_get_connection_fd(PlatformCtx *p)
{
	(void) p;
	return -1;
}

static void
stub_sync(PlatformCtx *p)
{
	(void) p;
}

static void
stub_free_pixmap(PlatformCtx *p, WmPixmap pm)
{
	(void) p;
	(void) pm;
}

static void
stub_free_colormap(PlatformCtx *p, WmColormap cm)
{
	(void) p;
	(void) cm;
}

static void
stub_intern_atoms_batch(
    PlatformCtx *p, const char **names, AtomId *atoms_out, int n)
{
	int i;

	(void) p;
	(void) names;
	if (!atoms_out)
		return;
	for (i = 0; i < n; i++)
		atoms_out[i] = ATOM_NONE;
}

static WinId
stub_create_window(PlatformCtx *p, WinId parent, int x, int y, int w, int h,
    uint32_t val_mask, const uint32_t *vals)
{
	(void) p;
	(void) parent;
	(void) x;
	(void) y;
	(void) w;
	(void) h;
	(void) val_mask;
	(void) vals;
	return WIN_NONE;
}

static void
stub_select_root_events(PlatformCtx *p, uint32_t mask)
{
	(void) p;
	(void) mask;
}

static void
stub_set_root_cursor(PlatformCtx *p, WmCursor cursor)
{
	(void) p;
	(void) cursor;
}

static void
stub_init_screen(PlatformCtx *p)
{
	if (!p)
		return;
	p->sw = 0;
	p->sh = 0;
}

static void
stub_keysyms_alloc(PlatformCtx *p)
{
	(void) p;
}

static void
stub_keysyms_free(PlatformCtx *p)
{
	(void) p;
}

static char *
stub_get_resource_manager(PlatformCtx *p)
{
	(void) p;
	return NULL;
}

PlatformCtx g_plat;

static WmBackend wm_backend_wayland_stub = {
	.flush                    = stub_flush,
	.configure_win            = stub_configure_win,
	.change_attr              = stub_change_attr,
	.map                      = stub_map,
	.unmap                    = stub_unmap,
	.destroy_win              = stub_destroy_win,
	.send_configure_notify    = stub_send_configure_notify,
	.set_input_focus          = stub_set_input_focus,
	.warp_pointer             = stub_warp_pointer,
	.allow_events             = stub_allow_events,
	.grab_pointer             = stub_grab_pointer,
	.ungrab_pointer           = stub_ungrab_pointer,
	.ungrab_button            = stub_ungrab_button,
	.query_pointer            = stub_query_pointer,
	.grab_server              = stub_grab_server,
	.ungrab_server            = stub_ungrab_server,
	.get_override_redirect    = stub_get_override_redirect,
	.get_event_mask           = stub_get_event_mask,
	.get_geometry             = stub_get_geometry,
	.is_window_descendant     = stub_is_window_descendant,
	.get_atom_prop            = stub_get_atom_prop,
	.get_text_prop            = stub_get_text_prop,
	.get_wm_icon              = stub_get_wm_icon,
	.get_wm_class             = stub_get_wm_class,
	.change_prop              = stub_change_prop,
	.delete_prop              = stub_delete_prop,
	.kill_client_hard         = stub_kill_client_hard,
	.change_save_set          = stub_change_save_set,
	.reparent_window          = stub_reparent_window,
	.create_bar_win           = stub_create_bar_win,
	.screen_depth             = stub_screen_depth,
	.get_wm_normal_hints      = stub_get_wm_normal_hints,
	.get_wm_hints             = stub_get_wm_hints,
	.set_wm_hints             = stub_set_wm_hints,
	.get_wm_transient_for     = stub_get_wm_transient_for,
	.grab_buttons             = stub_grab_buttons,
	.update_numlock_mask      = stub_update_numlock_mask,
	.grab_keys_full           = stub_grab_keys_full,
	.refresh_keyboard_mapping = stub_refresh_keyboard_mapping,
	.get_keysym               = stub_get_keysym,
	.check_other_wm           = stub_check_other_wm,
	.update_geom              = stub_update_geom,
	.poll_event               = stub_poll_event,
	.next_event               = stub_next_event,
	.grab_keyboard            = stub_grab_keyboard,
	.ungrab_keyboard          = stub_ungrab_keyboard,
	.ungrab_key               = stub_ungrab_key,
	.query_root_tree          = stub_query_root_tree,
	.get_window_attributes    = stub_get_window_attributes,
	.get_connection_fd        = stub_get_connection_fd,
	.sync                     = stub_sync,
	.free_pixmap              = stub_free_pixmap,
	.free_colormap            = stub_free_colormap,
	.intern_atoms_batch       = stub_intern_atoms_batch,
	.create_window            = stub_create_window,
	.select_root_events       = stub_select_root_events,
	.set_root_cursor          = stub_set_root_cursor,
	.init_screen              = stub_init_screen,
	.keysyms_alloc            = stub_keysyms_alloc,
	.keysyms_free             = stub_keysyms_free,
	.get_resource_manager     = stub_get_resource_manager,
};

WmBackend *g_wm_backend = &wm_backend_wayland_stub;
