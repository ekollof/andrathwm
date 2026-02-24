/* AndrathWM — awm-ui helper process
 * See LICENSE file for copyright and license details.
 *
 * awm-ui runs as a separate child process forked by awm at startup.  It owns
 * the launcher widget so that GTK grab state, Xlib connections, and potential
 * crashes are fully isolated from the window manager process.
 *
 * SNI context menus are now handled directly inside awm (see sni.c) via
 * gtk_menu_popup_at_pointer() on awm's own GTK main loop.  awm-ui only
 * handles the launcher.
 *
 * Communication with awm is over a Unix SOCK_SEQPACKET socket pair created
 * before the fork.  The child fd is passed in argv[1].  Each send()/recv() is
 * exactly one message: UiMsgHeader followed by payload_len bytes.
 *
 * Bulk messages (UI_MSG_PREVIEW_SHOW, UI_MSG_PREVIEW_UPDATE) carry a POSIX SHM
 * fd as SCM_RIGHTS ancillary data.  The header's payload_len describes the
 * byte size of the SHM segment.  The receiver mmap()s the fd, reads entries,
 * then closes and munmap()s it.
 */

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

#include <glib.h>
#include <gtk/gtk.h>
#include <gdk/gdkx.h>

#include "launcher.h"
#include "log.h"
#include "notif.h"
#include "preview.h"
#include "ui_proto.h"

/* Icon cache configuration — defined here since awm_ui doesn't include
 * config.h (which needs full WM types).  icon.c declares these extern. */
const unsigned int iconcachesize       = 128;
const unsigned int iconcachemaxentries = 128;

/* -------------------------------------------------------------------------
 * Global state
 * ---------------------------------------------------------------------- */

static int        ui_fd     = -1; /* socket fd to awm */
static GMainLoop *main_loop = NULL;
static Launcher  *launcher  = NULL;

/* Monitor workarea — updated by UI_MSG_MONITOR_GEOM */
static int mon_wx = 0, mon_wy = 0, mon_ww = 1920, mon_wh = 1080;

/* -------------------------------------------------------------------------
 * Send helpers
 * ---------------------------------------------------------------------- */

/* Send a fixed-size struct as payload.  Returns 0 on success, -1 on error. */
static int
ui_send(UiMsgType type, const void *payload, uint32_t len)
{
	uint8_t      buf[sizeof(UiMsgHeader) + UI_MSG_MAX_PAYLOAD];
	UiMsgHeader *hdr = (UiMsgHeader *) buf;

	if (len > UI_MSG_MAX_PAYLOAD) {
		awm_error("awm-ui: send: payload too large (%u)", len);
		return -1;
	}

	hdr->type        = (uint32_t) type;
	hdr->payload_len = len;
	if (len > 0 && payload)
		memcpy(buf + sizeof(UiMsgHeader), payload, len);

	ssize_t n = send(ui_fd, buf, sizeof(UiMsgHeader) + len, MSG_NOSIGNAL);
	if (n < 0) {
		awm_error("awm-ui: send failed: %s", strerror(errno));
		return -1;
	}
	return 0;
}

/* -------------------------------------------------------------------------
 * Launcher callback — called when the user activates a row
 * ---------------------------------------------------------------------- */

void
ui_launcher_exec_cb(const char *cmd)
{
	if (!cmd || !*cmd)
		return;

	size_t   len = strlen(cmd) + 1; /* include NUL */
	uint8_t *buf = malloc(len);
	if (!buf)
		return;
	memcpy(buf, cmd, len);
	ui_send(UI_MSG_LAUNCHER_EXEC, buf, (uint32_t) len);
	free(buf);
}

/* -------------------------------------------------------------------------
 * Message dispatch
 * ---------------------------------------------------------------------- */

static void
dispatch_launcher_show(const uint8_t *payload, uint32_t len)
{
	if (len < sizeof(UiLauncherShowPayload)) {
		awm_warn("awm-ui: LAUNCHER_SHOW payload too short");
		return;
	}
	UiLauncherShowPayload p;
	memcpy(&p, payload, sizeof(p));
	launcher_show(launcher, (int) p.x, (int) p.y);
}

static void
dispatch_monitor_geom(const uint8_t *payload, uint32_t len)
{
	UiMonitorGeomPayload p;

	if (len < sizeof(p)) {
		awm_warn("awm-ui: MONITOR_GEOM payload too short");
		return;
	}
	memcpy(&p, payload, sizeof(p));
	mon_wx = (int) p.wx;
	mon_wy = (int) p.wy;
	mon_ww = (int) p.ww;
	mon_wh = (int) p.wh;
	awm_debug(
	    "awm-ui: monitor geom %dx%d+%d+%d", mon_ww, mon_wh, mon_wx, mon_wy);
	notif_update_geom(mon_wx, mon_wy, mon_ww, mon_wh);
}

static void
handle_message(UiMsgType type, const uint8_t *payload, uint32_t len)
{
	switch (type) {
	case UI_MSG_LAUNCHER_SHOW:
		dispatch_launcher_show(payload, len);
		break;
	case UI_MSG_LAUNCHER_HIDE:
		launcher_hide(launcher);
		break;
	case UI_MSG_MONITOR_GEOM:
		dispatch_monitor_geom(payload, len);
		break;
	case UI_MSG_PREVIEW_HIDE:
		preview_hide();
		break;
	default:
		awm_warn("awm-ui: unknown message type %u", (unsigned) type);
		break;
	}
}

/* Handle a bulk SHM message (PREVIEW_SHOW / PREVIEW_UPDATE).
 * shm_fd is the POSIX SHM fd received via SCM_RIGHTS; shm_size is the
 * mapping byte length from header.payload_len.  This function owns shm_fd
 * and must close it before returning. */
static void
handle_shm_message(UiMsgType type, int shm_fd, size_t shm_size)
{
	void *base;

	base = mmap(NULL, shm_size, PROT_READ, MAP_SHARED, shm_fd, 0);
	close(shm_fd);
	if (base == MAP_FAILED) {
		awm_error("awm-ui: mmap SHM fd: %s", strerror(errno));
		return;
	}

	switch (type) {
	case UI_MSG_PREVIEW_SHOW:
	case UI_MSG_PREVIEW_UPDATE: {
		UiPreviewShowPayload hdr;
		if (shm_size < sizeof(hdr))
			break;
		memcpy(&hdr, base, sizeof(hdr));
		{
			unsigned int count = hdr.count;
			size_t       exp   = sizeof(hdr) + count * sizeof(UiPreviewEntry);
			const UiPreviewEntry *entries;
			if (shm_size < exp)
				break;
			entries = (const UiPreviewEntry *) ((const uint8_t *) base +
			    sizeof(hdr));
			preview_show(
			    entries, count, (int) hdr.anchor_x, (int) hdr.anchor_y);
		}
		break;
	}
	default:
		awm_warn("awm-ui: unexpected SHM message type %u", (unsigned) type);
		break;
	}

	munmap(base, shm_size);
}

/* -------------------------------------------------------------------------
 * GLib GSource for the socket fd
 * ---------------------------------------------------------------------- */

typedef struct {
	GSource source;
	GPollFD pfd;
} UiSocketSource;

static gboolean
socket_prepare(GSource *src, gint *timeout)
{
	(void) src;
	*timeout = -1;
	return FALSE;
}

static gboolean
socket_check(GSource *src)
{
	UiSocketSource *s = (UiSocketSource *) src;
	return (s->pfd.revents & (G_IO_IN | G_IO_HUP | G_IO_ERR)) != 0;
}

static gboolean
socket_dispatch(GSource *src, GSourceFunc cb, gpointer data)
{
	(void) cb;
	(void) data;
	UiSocketSource *s = (UiSocketSource *) src;

	if (s->pfd.revents & (G_IO_HUP | G_IO_ERR)) {
		awm_warn("awm-ui: socket closed/error — exiting");
		g_main_loop_quit(main_loop);
		return G_SOURCE_REMOVE;
	}

	/* Receive one message; use recvmsg so we can pick up any SCM_RIGHTS
	 * ancillary data carrying a SHM fd for bulk PREVIEW messages. */
	uint8_t       buf[sizeof(UiMsgHeader) + UI_MSG_MAX_PAYLOAD];
	uint8_t       cmsgbuf[CMSG_SPACE(sizeof(int))];
	struct iovec  iov   = { buf, sizeof(buf) };
	struct msghdr mhdr  = { 0 };
	mhdr.msg_iov        = &iov;
	mhdr.msg_iovlen     = 1;
	mhdr.msg_control    = cmsgbuf;
	mhdr.msg_controllen = sizeof(cmsgbuf);

	ssize_t n = recvmsg(ui_fd, &mhdr, 0);
	if (n <= 0) {
		if (n == 0 || errno == ECONNRESET) {
			awm_warn("awm-ui: socket disconnected — exiting");
		} else {
			awm_error("awm-ui: recvmsg: %s", strerror(errno));
		}
		g_main_loop_quit(main_loop);
		return G_SOURCE_REMOVE;
	}

	if ((size_t) n < sizeof(UiMsgHeader)) {
		awm_warn("awm-ui: short read (%zd bytes)", n);
		return G_SOURCE_CONTINUE;
	}

	UiMsgHeader hdr;
	memcpy(&hdr, buf, sizeof(hdr));

	/* Check for SCM_RIGHTS fd (bulk SHM path) */
	int shm_fd = -1;
	{
		struct cmsghdr *cm = CMSG_FIRSTHDR(&mhdr);
		if (cm && cm->cmsg_level == SOL_SOCKET &&
		    cm->cmsg_type == SCM_RIGHTS &&
		    cm->cmsg_len >= CMSG_LEN(sizeof(int))) {
			memcpy(&shm_fd, CMSG_DATA(cm), sizeof(int));
		}
	}

	if (shm_fd >= 0) {
		/* Bulk SHM message: payload_len is the SHM byte size */
		handle_shm_message(
		    (UiMsgType) hdr.type, shm_fd, (size_t) hdr.payload_len);
		return G_SOURCE_CONTINUE;
	}

	/* Inline payload */
	if (hdr.payload_len > UI_MSG_MAX_PAYLOAD) {
		awm_warn("awm-ui: payload_len %u exceeds cap", hdr.payload_len);
		return G_SOURCE_CONTINUE;
	}

	if ((size_t) n < sizeof(UiMsgHeader) + hdr.payload_len) {
		awm_warn("awm-ui: message truncated (got %zd want %zu)", n,
		    sizeof(UiMsgHeader) + hdr.payload_len);
		return G_SOURCE_CONTINUE;
	}

	handle_message(
	    (UiMsgType) hdr.type, buf + sizeof(UiMsgHeader), hdr.payload_len);

	return G_SOURCE_CONTINUE;
}

static GSourceFuncs socket_funcs = {
	socket_prepare,
	socket_check,
	socket_dispatch,
	NULL,
	NULL,
	NULL,
};

/* -------------------------------------------------------------------------
 * main
 * ---------------------------------------------------------------------- */

int
main(int argc, char *argv[])
{
	log_init("awm-ui");

	if (argc < 2) {
		fprintf(stderr, "awm-ui: usage: awm-ui <socket_fd>\n");
		return 1;
	}

	ui_fd = atoi(argv[1]);
	if (ui_fd < 0) {
		fprintf(stderr, "awm-ui: invalid socket fd: %s\n", argv[1]);
		return 1;
	}

	/* Ignore SIGPIPE — we detect broken socket via send() errno */
	signal(SIGPIPE, SIG_IGN);

	/* Initialise GTK */
	gtk_init(&argc, &argv);

	/* Create launcher (no XCB connection needed — runs in-process with GTK) */
	launcher = launcher_create(ui_fd, NULL);
	if (!launcher) {
		awm_error("awm-ui: failed to create launcher");
		return 1;
	}

	/* Start notification daemon */
	notif_init(mon_wx, mon_wy, mon_ww, mon_wh);

	/* Initialise preview popup module */
	preview_init(ui_fd);

	/* Register GLib socket source */
	UiSocketSource *sock_src =
	    (UiSocketSource *) g_source_new(&socket_funcs, sizeof(UiSocketSource));
	sock_src->pfd.fd      = ui_fd;
	sock_src->pfd.events  = G_IO_IN | G_IO_HUP | G_IO_ERR;
	sock_src->pfd.revents = 0;
	g_source_add_poll((GSource *) sock_src, &sock_src->pfd);
	g_source_attach((GSource *) sock_src, NULL);
	g_source_unref((GSource *) sock_src);

	/* Run */
	main_loop = g_main_loop_new(NULL, FALSE);
	g_main_loop_run(main_loop);

	/* Cleanup */
	g_main_loop_unref(main_loop);
	preview_cleanup();
	notif_cleanup();
	launcher_free(launcher);
	close(ui_fd);
	log_cleanup();
	return 0;
}
