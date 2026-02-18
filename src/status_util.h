/* AndrathWM - status module utilities
 * See LICENSE file for copyright and license details. */

#ifndef STATUS_UTIL_H
#define STATUS_UTIL_H

#include <stddef.h>
#include <stdint.h>

#define STATUS_LEN(x) (sizeof(x) / sizeof((x)[0]))

extern char status_buf[1024];

void status_warn(const char *fmt, ...);
int status_esnprintf(char *str, size_t size, const char *fmt, ...);
const char *status_bprintf(const char *fmt, ...);
const char *status_fmt_human(uintmax_t num, int base);
int status_pscanf(const char *path, const char *fmt, ...);

#endif /* STATUS_UTIL_H */
