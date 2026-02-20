/* x11_constants.h — inline copies of the handful of X11 protocol constants
 * and types that awm needs that have NO direct XCB equivalent.
 *
 * Everything else (event types, modifier masks, button indices) is now
 * referenced via the XCB_ names from <xcb/xproto.h>, which awm.h pulls in
 * via <xcb/xcb.h>.
 *
 * Values are taken directly from the X11 protocol specification and have
 * not changed since X11R1 (1987).
 *
 * See LICENSE file for copyright and license details. */

#ifndef X11_CONSTANTS_H
#define X11_CONSTANTS_H

#include <stdint.h>

/* KeySym — an X11 keysym is a 32-bit identifier (X.h: typedef XID KeySym,
 * where XID is unsigned long; Xproto.h redefines it as CARD32 = uint32_t).
 * We use uint32_t so it matches xcb_key_symbols_get_keysym() return type. */
typedef uint32_t KeySym;

/* LASTEvent — the highest X11 core event number + 1.  The handler[] dispatch
 * table in awm.c is sized [LASTEvent] and indexed by xcb event type.
 * Value is 36 per X11 protocol (X.h: #define LASTEvent 36). */
#define LASTEvent 36

/* X11 core request opcodes used in xcb_error_handler() to classify async
 * errors.  Values are from the X11 core protocol (Xproto.h).
 * XCB does not expose these as named constants. */
#define X_ConfigureWindow 12
#define X_GrabButton 28
#define X_GrabKey 33
#define X_SetInputFocus 42
#define X_CopyArea 62
#define X_PolySegment 66
#define X_PolyFillRectangle 70
#define X_PolyText8 74

#endif /* X11_CONSTANTS_H */
