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
 */

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <glib.h>
#include <gtk/gtk.h>
#include <gdk/gdkx.h>

#include "launcher.h"
#include "log.h"
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
handle_message(UiMsgType type, const uint8_t *payload, uint32_t len)
{
	switch (type) {
	case UI_MSG_LAUNCHER_SHOW:
		dispatch_launcher_show(payload, len);
		break;
	case UI_MSG_LAUNCHER_HIDE:
		launcher_hide(launcher);
		break;
	default:
		awm_warn("awm-ui: unknown message type %u", (unsigned) type);
		break;
	}
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

	/* Read one full message */
	uint8_t buf[sizeof(UiMsgHeader) + UI_MSG_MAX_PAYLOAD];
	ssize_t n = recv(ui_fd, buf, sizeof(buf), 0);
	if (n <= 0) {
		if (n == 0 || errno == ECONNRESET) {
			awm_warn("awm-ui: socket disconnected — exiting");
		} else {
			awm_error("awm-ui: recv: %s", strerror(errno));
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
	launcher_free(launcher);
	close(ui_fd);
	log_cleanup();
	return 0;
}
