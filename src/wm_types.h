/* See LICENSE file for copyright and license details. */

/* -------------------------------------------------------------------------
 * wm_types.h — platform-agnostic primitive types for core WM logic.
 *
 * This header defines backend-neutral type aliases and constants that
 * allow client.c, events.c, monitor.c, awm.h, platform.h, and render.h
 * to be written without direct references to xcb_* or wlr_* types.
 *
 * On BACKEND_X11:
 *   - WinId        == xcb_window_t     (uint32_t XID)
 *   - AtomId       == xcb_atom_t       (uint32_t)
 *   - KeySym       == xcb_keysym_t     (xkb_keysym_t, uint32_t)
 *   - WmKeycode    == xcb_keycode_t    (uint8_t, stored as uint32_t)
 *   - WmTimestamp  == xcb_timestamp_t  (uint32_t ms)
 *   - WmCursor     == xcb_cursor_t     (uint32_t XID)
 *   - WmPixmap     == xcb_pixmap_t     (uint32_t XID)
 *   - WmColormap   == xcb_colormap_t   (uint32_t XID)
 *   - WmVisualId   == xcb_visualid_t   (uint32_t)
 *
 * On BACKEND_WAYLAND (wlroots):
 *   - WinId holds a pointer to wlr_surface (cast via uintptr_t)
 *   - AtomId maps to xdg-shell protocol enums / xdg_atom stubs
 *   - KeySym / WmKeycode use xkbcommon natively (same bit layout)
 *   - WmCursor/WmPixmap/WmColormap are backend-internal handles
 *
 * The AWM_CW_*, AWM_CONFIG_WIN_*, AWM_EVENT_MASK_* and related
 * constants mirror the X11 wire values; the wlroots backend vtable
 * implementations simply ignore or translate them as needed.
 * ---------------------------------------------------------------------- */

#ifndef WM_TYPES_H
#define WM_TYPES_H

#include <stdint.h>

/* -------------------------------------------------------------------------
 * Primitive handle types
 * ---------------------------------------------------------------------- */

/* Window handle.  uintptr_t so it can hold either an xcb_window_t (32-bit
 * XID) or a pointer to a wlr_surface (pointer-sized on 64-bit). */
typedef uintptr_t WinId;
#define WIN_NONE ((WinId) 0)

/* Property/atom identifier.  uint32_t XID on X11; xdg-shell enum on
 * Wayland. */
typedef uint32_t AtomId;
#define ATOM_NONE ((AtomId) 0)

/* Key symbol.  Same bit layout as xkb_keysym_t and xcb_keysym_t.
 * Guarded because <X11/X.h> (pulled in transitively by GTK/GDK on BACKEND_X11)
 * defines KeySym as XID (unsigned long), which conflicts on 64-bit. */
#ifndef X_H
typedef uint32_t KeySym;
#endif /* X_H */

/* Raw keycode (evdev scancode on Wayland, X11 keycode on X11).
 * Widened to uint32_t so it fits both (X11 uses uint8_t). */
typedef uint32_t WmKeycode;

/* Event timestamp in milliseconds. */
typedef uint32_t WmTimestamp;
#define WM_CURRENT_TIME ((WmTimestamp) 0)

/* -------------------------------------------------------------------------
 * Modifier mask — backend-agnostic bitmask used in AwmEvent.key.state
 * and AwmEvent.button.state.
 *
 * The X11 backend translates XCB_MOD_MASK_* to these bits and back.
 * The wlroots backend translates WLR_MODIFIER_* similarly.
 * ---------------------------------------------------------------------- */
typedef uint32_t WmMods;
#define WM_MOD_SHIFT (1u << 0)   /* Shift */
#define WM_MOD_CAPS (1u << 1)    /* CapsLock */
#define WM_MOD_CTRL (1u << 2)    /* Control */
#define WM_MOD_ALT (1u << 3)     /* Mod1 / Alt */
#define WM_MOD_NUMLOCK (1u << 4) /* Mod2 / NumLock */
#define WM_MOD_MOD3 (1u << 5)    /* Mod3 */
#define WM_MOD_LOGO (1u << 6)    /* Mod4 / Super / Windows */
#define WM_MOD_MOD5 (1u << 7)    /* Mod5 / AltGr */

/* -------------------------------------------------------------------------
 * Opaque render/cursor handles — backend internal, never dereferenced
 * outside the backend implementation.
 * ---------------------------------------------------------------------- */
typedef uintptr_t WmCursor;   /* xcb_cursor_t / wlroots cursor handle */
typedef uintptr_t WmPixmap;   /* xcb_pixmap_t / wlroots buffer handle */
typedef uintptr_t WmColormap; /* xcb_colormap_t / unused on wlroots   */
typedef uint32_t  WmVisualId; /* xcb_visualid_t / wlroots format id   */

/* -------------------------------------------------------------------------
 * AWM_CW_* — window attribute change mask bits (match X11 wire values).
 * Passed as the mask argument to WmBackend.change_attr().
 * The wlroots backend ignores unsupported bits.
 * ---------------------------------------------------------------------- */
#define AWM_CW_BACK_PIXMAP 0x00000001u
#define AWM_CW_BACK_PIXEL 0x00000002u
#define AWM_CW_BORDER_PIXMAP 0x00000004u
#define AWM_CW_BORDER_PIXEL 0x00000010u
#define AWM_CW_BIT_GRAVITY 0x00000020u
#define AWM_CW_WIN_GRAVITY 0x00000040u
#define AWM_CW_BACKING_STORE 0x00000080u
#define AWM_CW_BACKING_PLANES 0x00000100u
#define AWM_CW_BACKING_PIXEL 0x00000200u
#define AWM_CW_OVERRIDE_REDIRECT 0x00000200u
#define AWM_CW_SAVE_UNDER 0x00000400u
#define AWM_CW_EVENT_MASK 0x00000800u
#define AWM_CW_DONT_PROPAGATE 0x00001000u
#define AWM_CW_COLORMAP 0x00002000u
#define AWM_CW_CURSOR 0x00004000u

/* -------------------------------------------------------------------------
 * AWM_CONFIG_WIN_* — configure window mask bits (match X11 wire values).
 * Passed as the mask argument to WmBackend.configure_win().
 * ---------------------------------------------------------------------- */
#define AWM_CONFIG_WIN_X 0x0001u
#define AWM_CONFIG_WIN_Y 0x0002u
#define AWM_CONFIG_WIN_WIDTH 0x0004u
#define AWM_CONFIG_WIN_HEIGHT 0x0008u
#define AWM_CONFIG_WIN_BORDER_WIDTH 0x0010u
#define AWM_CONFIG_WIN_SIBLING 0x0020u
#define AWM_CONFIG_WIN_STACK_MODE 0x0040u

/* -------------------------------------------------------------------------
 * AWM_STACK_MODE_* (match X11 wire values)
 * ---------------------------------------------------------------------- */
#define AWM_STACK_MODE_ABOVE 0u
#define AWM_STACK_MODE_BELOW 1u

/* -------------------------------------------------------------------------
 * AWM_EVENT_MASK_* — XCB event mask bits used in WmBackend.change_attr()
 * and WmBackend.send_configure_notify().  Match X11 wire values.
 * ---------------------------------------------------------------------- */
#define AWM_EVENT_MASK_NO_EVENT 0u
#define AWM_EVENT_MASK_KEY_PRESS (1u << 0)
#define AWM_EVENT_MASK_KEY_RELEASE (1u << 1)
#define AWM_EVENT_MASK_BUTTON_PRESS (1u << 2)
#define AWM_EVENT_MASK_BUTTON_RELEASE (1u << 3)
#define AWM_EVENT_MASK_ENTER_WINDOW (1u << 4)
#define AWM_EVENT_MASK_LEAVE_WINDOW (1u << 5)
#define AWM_EVENT_MASK_POINTER_MOTION (1u << 6)
#define AWM_EVENT_MASK_EXPOSURE (1u << 15)
#define AWM_EVENT_MASK_STRUCTURE_NOTIFY (1u << 17)
#define AWM_EVENT_MASK_RESIZE_REDIRECT (1u << 18)
#define AWM_EVENT_MASK_SUBSTRUCTURE_NOTIFY (1u << 19)
#define AWM_EVENT_MASK_SUBSTRUCTURE_REDIRECT (1u << 20)
#define AWM_EVENT_MASK_FOCUS_CHANGE (1u << 21)
#define AWM_EVENT_MASK_PROPERTY_CHANGE (1u << 22)

/* -------------------------------------------------------------------------
 * AWM_ALLOW_* — pointer event replay modes (match X11 wire values)
 * ---------------------------------------------------------------------- */
#define AWM_ALLOW_ASYNC_POINTER 1u
#define AWM_ALLOW_REPLAY_POINTER 3u

/* -------------------------------------------------------------------------
 * AWM_NOTIFY_MODE_* — enter/leave/focus notify modes (X11 wire values)
 * ---------------------------------------------------------------------- */
#define AWM_NOTIFY_MODE_NORMAL 0u
#define AWM_NOTIFY_MODE_GRAB 1u
#define AWM_NOTIFY_MODE_UNGRAB 2u
#define AWM_NOTIFY_MODE_WHILE_GRABBED 3u

/* -------------------------------------------------------------------------
 * AWM_NOTIFY_DETAIL_* — enter/leave detail values (X11 wire values)
 * ---------------------------------------------------------------------- */
#define AWM_NOTIFY_DETAIL_ANCESTOR 0u
#define AWM_NOTIFY_DETAIL_VIRTUAL 1u
#define AWM_NOTIFY_DETAIL_INFERIOR 2u
#define AWM_NOTIFY_DETAIL_NONLINEAR 3u
#define AWM_NOTIFY_DETAIL_NONLINEAR_VIRTUAL 4u
#define AWM_NOTIFY_DETAIL_POINTER 5u
#define AWM_NOTIFY_DETAIL_POINTER_ROOT 6u
#define AWM_NOTIFY_DETAIL_NONE 7u

/* -------------------------------------------------------------------------
 * AWM_PROPERTY_* — property state values (X11 wire values)
 * ---------------------------------------------------------------------- */
#define AWM_PROPERTY_NEW_VALUE 0u
#define AWM_PROPERTY_DELETE 1u

/* -------------------------------------------------------------------------
 * AWM_MAPPING_* — mapping notify request values (X11 wire values)
 * ---------------------------------------------------------------------- */
#define AWM_MAPPING_MODIFIER 0u
#define AWM_MAPPING_KEYBOARD 1u
#define AWM_MAPPING_POINTER 2u

/* -------------------------------------------------------------------------
 * AWM_PROP_MODE_* — property change modes (match X11 wire values)
 * ---------------------------------------------------------------------- */
#define AWM_PROP_MODE_REPLACE 0u
#define AWM_PROP_MODE_PREPEND 1u
#define AWM_PROP_MODE_APPEND 2u

/* -------------------------------------------------------------------------
 * AWM_ATOM_* — well-known atoms (X11 predefined values)
 * ---------------------------------------------------------------------- */
#define AWM_ATOM_NONE ((AtomId) 0)
#define AWM_ATOM_ANY ((AtomId) 0) /* XCB_ATOM_ANY == 0 */
#define AWM_ATOM_ATOM ((AtomId) 4)
#define AWM_ATOM_CARDINAL ((AtomId) 6)
#define AWM_ATOM_STRING ((AtomId) 31)
#define AWM_ATOM_WM_HINTS ((AtomId) 35)
#define AWM_ATOM_WM_NORMAL_HINTS ((AtomId) 40)
#define AWM_ATOM_WM_NAME ((AtomId) 39)
#define AWM_ATOM_WM_CLASS ((AtomId) 67)
#define AWM_ATOM_WM_TRANSIENT_FOR ((AtomId) 68)
#define AWM_ATOM_WINDOW ((AtomId) 33)

/* -------------------------------------------------------------------------
 * AWM_WM_STATE_* — ICCCM WM_STATE values
 * ---------------------------------------------------------------------- */
#define AWM_WM_STATE_WITHDRAWN 0
#define AWM_WM_STATE_NORMAL 1
#define AWM_WM_STATE_ICONIC 3

/* -------------------------------------------------------------------------
 * AWM_MAP_STATE_* — window map state values (X11 wire values)
 * ---------------------------------------------------------------------- */
#define AWM_MAP_STATE_UNMAPPED 0
#define AWM_MAP_STATE_UNVIEWABLE 1
#define AWM_MAP_STATE_VIEWABLE 2

/* -------------------------------------------------------------------------
 * AWM_INPUT_FOCUS_* — focus revert-to values (X11 wire values)
 * ---------------------------------------------------------------------- */
#define AWM_INPUT_FOCUS_NONE 0
#define AWM_INPUT_FOCUS_POINTER_ROOT 1
#define AWM_INPUT_FOCUS_PARENT 2

/* -------------------------------------------------------------------------
 * AwmSizeHints — platform-agnostic size constraints
 * (ICCCM WM_NORMAL_HINTS subset used by core WM logic)
 * ---------------------------------------------------------------------- */
typedef struct {
	int      min_w, min_h;
	int      max_w, max_h;
	int      base_w, base_h;
	int      inc_w, inc_h;
	double   min_aspect;
	double   max_aspect;
	unsigned flags;
} AwmSizeHints;

/* Flag bits for AwmSizeHints.flags */
#define AWM_SIZE_P_MIN_SIZE (1u << 0)
#define AWM_SIZE_P_MAX_SIZE (1u << 1)
#define AWM_SIZE_P_RESIZE_INC (1u << 2)
#define AWM_SIZE_P_ASPECT (1u << 3)
#define AWM_SIZE_P_BASE_SIZE (1u << 4)
#define AWM_SIZE_P_SIZE (1u << 5)

/* -------------------------------------------------------------------------
 * AwmWmHints — platform-agnostic WM hints
 * (ICCCM WM_HINTS subset used by core WM logic)
 * ---------------------------------------------------------------------- */
typedef struct {
	int      input; /* 1 = client requests keyboard input */
	unsigned flags;
} AwmWmHints;

/* Flag bits for AwmWmHints.flags */
#define AWM_WM_HINT_INPUT (1u << 0)
#define AWM_WM_HINT_URGENCY (1u << 1)

/* -------------------------------------------------------------------------
 * AwmEvent — platform-agnostic event delivered to handler[]
 *
 * The backend (platform_x11.c / platform_wlroots.c) translates its
 * native event type into an AwmEvent before calling handler[type]().
 * Core WM logic (events.c) reads only the AwmEvent fields.
 * ---------------------------------------------------------------------- */
typedef enum {
	AWM_EV_NONE = 0,
	AWM_EV_BUTTON_PRESS,
	AWM_EV_BUTTON_RELEASE,
	AWM_EV_KEY_PRESS,
	AWM_EV_KEY_RELEASE,
	AWM_EV_ENTER,
	AWM_EV_LEAVE,
	AWM_EV_FOCUS_IN,
	AWM_EV_MOTION,
	AWM_EV_EXPOSE,
	AWM_EV_CONFIGURE_NOTIFY,
	AWM_EV_CONFIGURE_REQUEST,
	AWM_EV_MAP_REQUEST,
	AWM_EV_UNMAP_NOTIFY,
	AWM_EV_DESTROY_NOTIFY,
	AWM_EV_PROPERTY_NOTIFY,
	AWM_EV_CLIENT_MESSAGE,
	AWM_EV_RESIZE_REQUEST,
	AWM_EV_MAPPING_NOTIFY,
} AwmEventType;

typedef struct {
	AwmEventType type;
	WinId        window; /* primary window for this event */
	union {
		/* AWM_EV_BUTTON_PRESS / AWM_EV_BUTTON_RELEASE */
		struct {
			uint8_t     button; /* button index (1-5) */
			WmMods      state;  /* modifier mask */
			WmTimestamp time;
			int         root_x, root_y;
			int event_x, event_y; /* pointer coords relative to event window */
		} button;

		/* AWM_EV_KEY_PRESS / AWM_EV_KEY_RELEASE */
		struct {
			WmKeycode   keycode;
			WmMods      state;
			WmTimestamp time;
		} key;

		/* AWM_EV_CONFIGURE_NOTIFY */
		struct {
			int   x, y, w, h, bw;
			WinId above;
			int   is_send_event;
		} configure;

		/* AWM_EV_CONFIGURE_REQUEST */
		struct {
			uint16_t value_mask; /* AWM_CONFIG_WIN_* bits */
			int      x, y, w, h, bw;
			WinId    sibling;
			uint8_t  stack_mode; /* AWM_STACK_MODE_* */
		} configure_req;

		/* AWM_EV_ENTER / AWM_EV_LEAVE / AWM_EV_FOCUS_IN */
		struct {
			uint8_t mode; /* NotifyNormal / NotifyGrab etc. */
			uint8_t detail;
		} enter_leave_focus;

		/* AWM_EV_EXPOSE */
		struct {
			uint16_t count;
		} expose;

		/* AWM_EV_MAP_REQUEST */
		struct {
			WinId parent;
		} map_request;

		/* AWM_EV_UNMAP_NOTIFY / AWM_EV_DESTROY_NOTIFY */
		struct {
			WinId event; /* event window (may differ from window) */
			int   is_send_event;
		} unmap_destroy;

		/* AWM_EV_MOTION */
		struct {
			WmTimestamp time;
			int         root_x, root_y;
			uint8_t     detail; /* pointer type */
		} motion;

		/* AWM_EV_PROPERTY_NOTIFY */
		struct {
			AtomId  atom;
			uint8_t state; /* 0=NewValue, 1=Deleted */
		} property;

		/* AWM_EV_CLIENT_MESSAGE */
		struct {
			AtomId   msg_type;
			uint32_t data[5];
		} client_message;

		/* AWM_EV_RESIZE_REQUEST */
		struct {
			int w, h;
		} resize_request;

		/* AWM_EV_MAPPING_NOTIFY */
		struct {
			uint8_t request;
			uint8_t first_keycode;
			uint8_t count;
		} mapping;
	};
} AwmEvent;

#endif /* WM_TYPES_H */
