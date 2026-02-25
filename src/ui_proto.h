/* AndrathWM — IPC protocol between awm and awm-ui
 * See LICENSE file for copyright and license details.
 *
 * Transport: Unix SOCK_SEQPACKET socket.  Each send()/recv() is exactly one
 * message.  The first sizeof(UiMsgHeader) bytes are the fixed header; the
 * remaining bytes (header.payload_len) are the variable payload defined per
 * message type below.
 *
 * For messages that carry bulk data (PREVIEW_SHOW, PREVIEW_UPDATE), the
 * payload is written into a POSIX SHM segment and the file descriptor is
 * passed as SCM_RIGHTS ancillary data alongside the message.  The header's
 * payload_len field then describes the byte size of the SHM mapping.
 * Ordinary messages (no SHM fd) continue to use the inline payload path.
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
	UI_MSG_LAUNCHER_SHOW  = 1, /* payload: UiLauncherShowPayload        */
	UI_MSG_LAUNCHER_HIDE  = 2, /* payload: none (payload_len == 0)      */
	UI_MSG_MONITOR_GEOM   = 3, /* payload: UiMonitorGeomPayload         */
	UI_MSG_PREVIEW_SHOW   = 4, /* payload_len = SHM size; fd via SCM    */
	UI_MSG_PREVIEW_HIDE   = 5, /* payload: none                         */
	UI_MSG_PREVIEW_UPDATE = 6, /* payload_len = SHM size; fd via SCM    */
	UI_MSG_THEME          = 7, /* payload: UiThemePayload               */

	/* awm-ui → awm */
	UI_MSG_LAUNCHER_EXEC      = 10, /* payload: NUL-terminated cmd string */
	UI_MSG_PREVIEW_FOCUS      = 11, /* payload: UiPreviewFocusPayload     */
	UI_MSG_PREVIEW_DONE       = 12, /* payload: UiPreviewDonePayload      */
	UI_MSG_LAUNCHER_DISMISSED = 13, /* payload: none — launcher hidden    */
	UI_MSG_LAUNCHER_READY     = 14, /* payload: UiLauncherReadyPayload    */
	UI_MSG_LAUNCHER_SHOWN =
	    15, /* payload: UiLauncherShownPayload (x/y reserved) */
} UiMsgType;

/* -------------------------------------------------------------------------
 * Wire header — precedes every message
 * ---------------------------------------------------------------------- */

typedef struct {
	uint32_t type;        /* UiMsgType */
	uint32_t payload_len; /* bytes following this header (or SHM size)  */
} UiMsgHeader;

#define UI_MSG_MAX_PAYLOAD \
	4096 /* sanity cap for inline payloads; SHM messages are unlimited */

/* -------------------------------------------------------------------------
 * awm → awm-ui payloads
 * ---------------------------------------------------------------------- */

/* UI_MSG_LAUNCHER_SHOW — carries the target monitor workarea so awm-ui can
 * centre the launcher using the actual scaled window size. */
typedef struct {
	int32_t wx; /* monitor workarea origin x */
	int32_t wy; /* monitor workarea origin y */
	int32_t ww; /* monitor workarea width    */
	int32_t wh; /* monitor workarea height   */
} UiLauncherShowPayload;

/* UI_MSG_MONITOR_GEOM — sent on startup and whenever monitor geometry
 * changes.  Describes the primary (selmon) monitor's workarea so awm-ui
 * can position popups correctly (bar height already subtracted). */
typedef struct {
	int32_t wx; /* workarea origin x */
	int32_t wy; /* workarea origin y */
	int32_t ww; /* workarea width    */
	int32_t wh; /* workarea height   */
} UiMonitorGeomPayload;

/* UI_MSG_PREVIEW_SHOW / UI_MSG_PREVIEW_UPDATE
 *
 * The message header's payload_len carries the total byte size of the SHM
 * segment.  The segment layout is:
 *
 *   UiPreviewShowPayload hdr     (fixed, at offset 0)
 *   UiPreviewEntry       [0]     (at offset sizeof(UiPreviewShowPayload))
 *   UiPreviewEntry       [1]
 *   ...
 *   UiPreviewEntry       [hdr.count - 1]
 *
 * The SHM fd is passed as SCM_RIGHTS ancillary data.  The receiver mmap()s
 * the fd, reads the entries, then closes and munmap()s it.  The snapshot
 * pixmap XIDs are owned by the receiver; it must return them via
 * UI_MSG_PREVIEW_DONE so awm can call xcb_free_pixmap(). */

typedef struct {
	int32_t  anchor_x; /* hint: popup anchor point (bar button centre) */
	int32_t  anchor_y;
	uint32_t count; /* number of UiPreviewEntry structs that follow  */
} UiPreviewShowPayload;

/* One entry per candidate window. */
typedef struct {
	uint32_t xwin;       /* XCB window ID                              */
	uint32_t pixmap_xid; /* XComposite snapshot pixmap XID, 0 = none  */
	int32_t  w;          /* window width  (used for aspect ratio)      */
	int32_t  h;          /* window height                              */
	uint8_t  depth;      /* pixmap colour depth (24 or 32)             */
	uint8_t  selected;   /* 1 = this window currently has focus        */
	uint8_t  _pad[2];
	char     title[64];     /* UTF-8 window title, NUL-terminated         */
	char     icon_name[64]; /* icon name or empty string                  */
} UiPreviewEntry;

/* UI_MSG_THEME — sent on startup and after every xrdb reload.
 * Colors are 16-bit per channel (matching the Clr struct).
 * font[] is a NUL-terminated Pango font description string (e.g.
 * "BerkeleyMono Nerd Font 12").
 * dpi is the resolved screen DPI (e.g. 96.0, 192.0). */
typedef struct {
	uint16_t norm_fg[4]; /* SchemeNorm ColFg  — r,g,b,a */
	uint16_t norm_bg[4]; /* SchemeNorm ColBg  — r,g,b,a */
	uint16_t norm_bd[4]; /* SchemeNorm ColBorder */
	uint16_t sel_fg[4];  /* SchemeSel  ColFg  */
	uint16_t sel_bg[4];  /* SchemeSel  ColBg  */
	uint16_t sel_bd[4];  /* SchemeSel  ColBorder */
	char     font[256];  /* Pango font description string, NUL-terminated */
	double   dpi;        /* resolved screen DPI (96.0 default) */
} UiThemePayload;

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

/* UI_MSG_LAUNCHER_SHOWN: sent by awm-ui after gtk_widget_show_all() +
 * gdk_display_sync().  Signals that the window is now mapped; awm sets X
 * input focus in response.  The x/y fields are reserved (set to 0) — awm
 * pre-positions the window before sending UI_MSG_LAUNCHER_SHOW. */
typedef struct {
	int32_t x; /* reserved — always 0 */
	int32_t y; /* reserved — always 0 */
} UiLauncherShownPayload;

/* UI_MSG_PREVIEW_FOCUS — user clicked a card; awm should focus that window. */
typedef struct {
	uint32_t xwin; /* XCB window ID to focus */
} UiPreviewFocusPayload;

/* UI_MSG_PREVIEW_DONE — preview is done; free the listed snapshot pixmaps. */
typedef struct {
	uint32_t count;    /* number of XIDs in xids[] */
	uint32_t xids[32]; /* snapshot pixmap XIDs to free */
} UiPreviewDonePayload;

/* -------------------------------------------------------------------------
 * Helper: compute total inline message size
 * ---------------------------------------------------------------------- */

#define UI_MSG_TOTAL(payload_len) \
	(sizeof(UiMsgHeader) + (size_t) (payload_len))

#endif /* UI_PROTO_H */
