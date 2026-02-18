/* See LICENSE file for copyright and license details. */
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <X11/Xlib.h>

#include "arg.h"
#include "slstatus.h"
#include "util.h"

/* Forward declarations for component functions we need to reference directly */
const char *cpu_perc(const char *unused);
const char *battery_state(const char *bat);
const char *battery_perc(const char *bat);

/* Custom function to show better battery status */
const char *
battery_status(const char *bat)
{
	const char *state = battery_state(bat);
	const char *perc = battery_perc(bat);
	static char status_buf[64];
	
	if (!state || !perc)
		return NULL;
		
	/* Convert the state symbol into a descriptive emoji */
	const char *icon;
	if (state[0] == '+') 
		icon = "⚡"; /* Charging */
	else if (state[0] == '-')
		icon = "🔋"; /* Discharging */
	else if (state[0] == 'o')
		icon = "🔌"; /* Full or not charging */
	else
		icon = "❓"; /* Unknown state */
	
	/* Format the output with icon and percentage */
	snprintf(status_buf, sizeof(status_buf), "%s %s", icon, perc);
	return status_buf;
}

struct arg {
	const char *(*func)(const char *);
	const char *fmt;
	const char *args;
	unsigned int interval;  /* interval in seconds to call this function */
};

char buf[1024];
static volatile sig_atomic_t done;
static Display *dpy;

#include "config.h"

static void
terminate(const int signo)
{
	if (signo != SIGUSR1)
		done = 1;
}

static void
difftimespec(struct timespec *res, struct timespec *a, struct timespec *b)
{
	res->tv_sec = a->tv_sec - b->tv_sec - (a->tv_nsec < b->tv_nsec);
	res->tv_nsec = a->tv_nsec - b->tv_nsec +
	               (a->tv_nsec < b->tv_nsec) * 1E9;
}

static void
usage(void)
{
	die("usage: %s [-v] [-s] [-1]", argv0);
}

int
main(int argc, char *argv[])
{
	struct sigaction act;
	struct timespec start, current, diff, intspec, wait;
	size_t i, len;
	int sflag, ret;
	char status[MAXLEN];
	const char *res;
	time_t last_update_time[LEN(args)];
	time_t current_time;
	char cached_results[LEN(args)][256]; /* Store cached results for each component */

	/* Initialize all components with 0 timestamp to run them on first iteration */
	memset(last_update_time, 0, sizeof(last_update_time));
	memset(cached_results, 0, sizeof(cached_results));
    
	/* Initialize cached results with unknown_str to avoid empty initial values */
	for (i = 0; i < LEN(args); i++) {
		strncpy(cached_results[i], unknown_str, sizeof(cached_results[i]) - 1);
		cached_results[i][sizeof(cached_results[i]) - 1] = '\0';
	}

	sflag = 0;
	ARGBEGIN {
	case 'v':
		die("slstatus-"VERSION);
		break; /* Not reached but silences fallthrough warning */
	case '1':
		done = 1;
		/* FALLTHROUGH */
	case 's':
		sflag = 1;
		break;
	default:
		usage();
	} ARGEND

	if (argc)
		usage();

	/* Setup signal handlers */
	memset(&act, 0, sizeof(act));
	act.sa_handler = terminate;
	sigaction(SIGINT,  &act, NULL);
	sigaction(SIGTERM, &act, NULL);
	act.sa_flags |= SA_RESTART;
	sigaction(SIGUSR1, &act, NULL);

	/* Only open X11 display if not in stdout mode */
	if (!sflag && !(dpy = XOpenDisplay(NULL)))
		die("XOpenDisplay: Failed to open display");

	/* Initialize components that need multiple samples */
	for (i = 0; i < LEN(args); i++) {
		if (args[i].func == cpu_perc) {
			/* Prime CPU readings and store initial value */
			res = args[i].func(args[i].args);
			if (res) {
				strncpy(cached_results[i], res, sizeof(cached_results[i]) - 1);
				cached_results[i][sizeof(cached_results[i]) - 1] = '\0';
			}
		}
	}
	
	/* Add a small delay to allow cpu_perc to get a meaningful reading */
	struct timespec ts = { .tv_sec = 0, .tv_nsec = 500000000 }; /* 500ms delay */
	nanosleep(&ts, NULL);
    
	/* Get initial readings from all components */
	current_time = time(NULL);
	for (i = 0; i < LEN(args); i++) {
		res = args[i].func(args[i].args);
		if (res) {
			strncpy(cached_results[i], res, sizeof(cached_results[i]) - 1);
			cached_results[i][sizeof(cached_results[i]) - 1] = '\0';
		}
		last_update_time[i] = current_time;
	}

	do {
		if (clock_gettime(CLOCK_MONOTONIC, &start) < 0)
			die("clock_gettime:");
		
		current_time = time(NULL);
		status[0] = '\0';
		len = 0;
		
		/* Generate status string - identical for both modes */
		for (i = 0; i < LEN(args); i++) {
			/* Only update if interval seconds have passed since last update */
			if (current_time - last_update_time[i] >= args[i].interval) {
				res = args[i].func(args[i].args);
				if (res) {
					/* Only update cache if we got a valid result */
					strncpy(cached_results[i], res, sizeof(cached_results[i]) - 1);
					cached_results[i][sizeof(cached_results[i]) - 1] = '\0';
				}
				/* Always update the timestamp, even if result was NULL */
				last_update_time[i] = current_time;
			}
            
			/* Always use cached result - it will be either a valid result or unknown_str */
			res = cached_results[i];

			/* Format the result using the component's format string */
			ret = snprintf(buf, sizeof(buf), args[i].fmt, res);
			if (ret < 0 || (size_t)ret >= sizeof(buf))
				continue;

			/* Add the component output to the status string */
			if (len + ret < sizeof(status) - 1) {
				memcpy(status + len, buf, ret);
				len += ret;
				status[len] = '\0';
			}
		}

		/* Output the status string based on mode */
		if (sflag) {
			/* Stdout mode */
			printf("%s\n", status);
			fflush(stdout);
		} else {
			/* X11 mode */
			if (XStoreName(dpy, DefaultRootWindow(dpy), status) < 0)
				die("XStoreName: Allocation failed");
			XFlush(dpy);
		}
		
		/* Exit after first iteration if one-shot mode */
		if (done)
			break;

		/* Sleep until next update interval */
		if (clock_gettime(CLOCK_MONOTONIC, &current) < 0)
			die("clock_gettime:");
		difftimespec(&diff, &current, &start);

		intspec.tv_sec = interval / 1000;
		intspec.tv_nsec = (interval % 1000) * 1E6;
		difftimespec(&wait, &intspec, &diff);

		if (wait.tv_sec >= 0 &&
			nanosleep(&wait, NULL) < 0 &&
			errno != EINTR)
				die("nanosleep:");
	} while (!done);

	/* Clean up X11 resources */
	if (!sflag) {
		XStoreName(dpy, DefaultRootWindow(dpy), NULL);
		if (XCloseDisplay(dpy) < 0)
			die("XCloseDisplay: Failed to close display");
	}

	return 0;
}
