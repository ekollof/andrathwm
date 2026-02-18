/* Simple utility to query X11 idle time using XScreenSaver extension
 * Compile: gcc -o xidle xidle.c -lX11 -lXss
 * Usage: ./xidle [-h]
 *   No args: Print idle time in milliseconds
 *   -h:      Print idle time in human-readable format
 */
#include <X11/Xlib.h>
#include <X11/extensions/scrnsaver.h>
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
	Display          *dpy;
	XScreenSaverInfo *info;
	int               event_base, error_base;
	int               human_readable = 0;

	/* Parse arguments */
	if (argc > 1 && strcmp(argv[1], "-h") == 0) {
		human_readable = 1;
	}

	dpy = XOpenDisplay(NULL);
	if (!dpy) {
		fprintf(stderr, "xidle: cannot open display\n");
		return 1;
	}

	/* Check if XScreenSaver extension is available */
	if (!XScreenSaverQueryExtension(dpy, &event_base, &error_base)) {
		fprintf(stderr, "xidle: XScreenSaver extension not available\n");
		XCloseDisplay(dpy);
		return 1;
	}

	info = XScreenSaverAllocInfo();
	if (!info) {
		fprintf(stderr, "xidle: failed to allocate XScreenSaverInfo\n");
		XCloseDisplay(dpy);
		return 1;
	}

	if (XScreenSaverQueryInfo(dpy, DefaultRootWindow(dpy), info)) {
		if (human_readable) {
			print_human_time(info->idle);
		} else {
			/* idle time is in milliseconds */
			printf("%lu\n", info->idle);
		}
	} else {
		fprintf(stderr, "xidle: failed to query idle time\n");
		XFree(info);
		XCloseDisplay(dpy);
		return 1;
	}

	XFree(info);
	XCloseDisplay(dpy);
	return 0;
}
