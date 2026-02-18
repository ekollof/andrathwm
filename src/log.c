/* See LICENSE file for copyright and license details. */
#include <stdarg.h>
#include <stdio.h>
#include <syslog.h>

#include "log.h"

static int syslog_initialized = 0;

void
log_init(const char *ident)
{
	openlog(ident, LOG_PID | LOG_CONS, LOG_USER);
	syslog_initialized = 1;
}

void
log_cleanup(void)
{
	if (syslog_initialized) {
		closelog();
		syslog_initialized = 0;
	}
}

static void
log_message(int syslog_level, const char *level_str, const char *func,
    int line, const char *fmt, va_list args)
{
	va_list args_copy;

	/* Log to stderr */
	fprintf(stderr, "awm: %s: %s:%d: ", level_str, func, line);
	va_copy(args_copy, args);
	vfprintf(stderr, fmt, args_copy);
	va_end(args_copy);
	fprintf(stderr, "\n");

	/* Log to syslog if initialized */
	if (syslog_initialized) {
		char msg[512];
		vsnprintf(msg, sizeof(msg), fmt, args);
		syslog(syslog_level, "%s:%d: %s", func, line, msg);
	}
}

void
log_debug(const char *func, int line, const char *fmt, ...)
{
#ifdef AWM_DEBUG
	va_list args;
	va_start(args, fmt);
	log_message(LOG_DEBUG, "debug", func, line, fmt, args);
	va_end(args);
#else
	(void) func;
	(void) line;
	(void) fmt;
#endif
}

void
log_info(const char *func, int line, const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	log_message(LOG_INFO, "info", func, line, fmt, args);
	va_end(args);
}

void
log_warn(const char *func, int line, const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	log_message(LOG_WARNING, "warning", func, line, fmt, args);
	va_end(args);
}

void
log_error(const char *func, int line, const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	log_message(LOG_ERR, "error", func, line, fmt, args);
	va_end(args);
}
