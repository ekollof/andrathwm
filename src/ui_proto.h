/* AndrathWM — IPC protocol between awm and awm-ui
 * See LICENSE file for copyright and license details.
 *
 * Transport: Unix SOCK_SEQPACKET socket.  Each send()/recv() is exactly one
 * message.  The first sizeof(UiMsgHeader) bytes are the fixed header; the
 * remaining bytes (header.payload_len) are the variable payload defined per
 * message type below.
 *
 * All integers are native byte order (both ends are the same process image).
 */

#ifndef UI_PROTO_H
#define UI_PROTO_H

#include <stdint.h>

/* -------------------------------------------------------------------------
 * Message types
 * ---------------------------------------------------------------------- */

typedef enum {
	/* awm → awm-ui */
	UI_MSG_LAUNCHER_SHOW = 1, /* payload: UiLauncherShowPayload  */
	UI_MSG_LAUNCHER_HIDE = 2, /* payload: none (payload_len == 0) */

	/* awm-ui → awm */
	UI_MSG_LAUNCHER_EXEC = 10, /* payload: NUL-terminated command string */
	UI_MSG_LAUNCHER_DISMISSED = 13, /* payload: none — launcher hidden */
	UI_MSG_LAUNCHER_READY =
	    14, /* payload: UiLauncherReadyPayload — sent once on startup */
} UiMsgType;

/* -------------------------------------------------------------------------
 * Wire header — precedes every message
 * ---------------------------------------------------------------------- */

typedef struct {
	uint32_t type;        /* UiMsgType */
	uint32_t payload_len; /* bytes following this header */
} UiMsgHeader;

#define UI_MSG_MAX_PAYLOAD \
	4096 /* sanity cap; largest payload is a command string */

/* -------------------------------------------------------------------------
 * awm → awm-ui payloads
 * ---------------------------------------------------------------------- */

/* UI_MSG_LAUNCHER_SHOW */
typedef struct {
	int32_t x;
	int32_t y;
} UiLauncherShowPayload;

/* -------------------------------------------------------------------------
 * awm-ui → awm payloads
 * ---------------------------------------------------------------------- */

/* UI_MSG_LAUNCHER_EXEC: payload is a NUL-terminated UTF-8 command string,
 * length = payload_len (includes the NUL byte). */

/* UI_MSG_LAUNCHER_READY: sent once by awm-ui after the launcher window is
 * realized, so awm can call xcb_set_input_focus directly on show. */
typedef struct {
	uint32_t xwin; /* X11 window ID of the launcher GdkWindow */
} UiLauncherReadyPayload;

/* -------------------------------------------------------------------------
 * Helper: compute total message size
 * ---------------------------------------------------------------------- */

#define UI_MSG_TOTAL(payload_len) \
	(sizeof(UiMsgHeader) + (size_t) (payload_len))

#endif /* UI_PROTO_H */
