/* AndrathWM - embedded status configuration
 * See LICENSE file for copyright and license details. */

#ifndef STATUS_CONFIG_H
#define STATUS_CONFIG_H

#include "status_components.h"

struct status_arg {
	const char *(*func)(const char *);
	const char  *fmt;
	const char  *args;
	unsigned int interval; /* interval in seconds to call this function */
	int          prime;    /* 1 = call once at startup to seed initial state */
};

/* interval between updates (in ms) */
static const unsigned int status_interval_ms = 1000;

/* text to show if no value can be retrieved */
static const char status_unknown_str[] = "n/a";

/* maximum output string length */
#define STATUS_MAXLEN 2048

static const struct status_arg status_args[] = {
	/* function format          argument    interval (seconds) prime */
	{ load_avg, "🖥 %s ", NULL, 5, 0 },
	/* Use our custom battery_status function for better charging indicators */
	{ battery_status, " %s ", "BAT0", 30, 0 },
	{ ram_used, "🐏 %s", NULL, 10, 0 },
	{ ram_total, "/%s ", NULL, 60, 0 },
	{ cpu_perc, "🔲 %s%% ", NULL, 2,
	    1 }, /* prime=1: seed /proc/stat at startup */
	{ datetime, "%s", "📆 %a %b %d 🕖 %H:%M:%S ", 1, 0 },
};

#define STATUS_ARGS_LEN (sizeof(status_args) / sizeof(status_args[0]))

#endif /* STATUS_CONFIG_H */
