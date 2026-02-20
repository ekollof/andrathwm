/* Simple utility to query X11 idle time using XCB screensaver extension
 * Compile: clang -std=c11 -o xidle xidle.c -lxcb -lxcb-screensaver
 * Usage: ./xidle [-h]
 *   No args: Print idle time in milliseconds
 *   -h:      Print idle time in human-readable format
 */
#include <xcb/xcb.h>
#include <xcb/screensaver.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void
print_human_time(unsigned long ms)
{
	unsigned long seconds = ms / 1000;
	unsigned long minutes = seconds / 60;
	unsigned long hours   = minutes / 60;
	unsigned long days    = hours / 24;

	if (days > 0) {
		printf("%lud %luh %lum %lus\n", days, hours % 24, minutes % 60,
		    seconds % 60);
	} else if (hours > 0) {
		printf("%luh %lum %lus\n", hours, minutes % 60, seconds % 60);
	} else if (minutes > 0) {
		printf("%lum %lus\n", minutes, seconds % 60);
	} else {
		printf("%lus\n", seconds);
	}
}

int
main(int argc, char *argv[])
{
	xcb_connection_t                   *xc;
	xcb_screen_t                       *screen;
	const xcb_query_extension_reply_t  *ext;
	xcb_screensaver_query_info_cookie_t ck;
	xcb_screensaver_query_info_reply_t *info;
	int                                 human_readable = 0;

	/* Parse arguments */
	if (argc > 1 && strcmp(argv[1], "-h") == 0) {
		human_readable = 1;
	}

	xc = xcb_connect(NULL, NULL);
	if (xcb_connection_has_error(xc)) {
		fprintf(stderr, "xidle: cannot open display\n");
		xcb_disconnect(xc);
		return 1;
	}

	/* Check if screensaver extension is available */
	ext = xcb_get_extension_data(xc, &xcb_screensaver_id);
	if (!ext || !ext->present) {
		fprintf(stderr, "xidle: XScreenSaver extension not available\n");
		xcb_disconnect(xc);
		return 1;
	}

	screen = xcb_setup_roots_iterator(xcb_get_setup(xc)).data;

	ck   = xcb_screensaver_query_info(xc, screen->root);
	info = xcb_screensaver_query_info_reply(xc, ck, NULL);
	if (!info) {
		fprintf(stderr, "xidle: failed to query idle time\n");
		xcb_disconnect(xc);
		return 1;
	}

	if (human_readable) {
		print_human_time(info->ms_since_user_input);
	} else {
		/* idle time is in milliseconds */
		printf("%u\n", info->ms_since_user_input);
	}

	free(info);
	xcb_disconnect(xc);
	return 0;
}
