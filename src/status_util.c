/* AndrathWM - status module utilities
 * See LICENSE file for copyright and license details. */

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "log.h"
#include "status_util.h"

char status_buf[1024];

void
status_warn(const char *fmt, ...)
{
	va_list ap;
	char    msg[256];
	int     ret;

	va_start(ap, fmt);
	ret = vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);

	if (ret < 0)
		return;
	awm_warn("%s", msg);
}

static int
status_vsnprintf(char *str, size_t size, const char *fmt, va_list ap)
{
	int ret;

	ret = vsnprintf(str, size, fmt, ap);
	if (ret < 0) {
		status_warn("vsnprintf failed");
		return -1;
	}
	if ((size_t) ret >= size) {
		status_warn("vsnprintf output truncated");
		return -1;
	}

	return ret;
}

int
status_esnprintf(char *str, size_t size, const char *fmt, ...)
{
	va_list ap;
	int     ret;

	va_start(ap, fmt);
	ret = status_vsnprintf(str, size, fmt, ap);
	va_end(ap);

	return ret;
}

const char *
status_bprintf(const char *fmt, ...)
{
	va_list ap;
	int     ret;

	va_start(ap, fmt);
	ret = status_vsnprintf(status_buf, sizeof(status_buf), fmt, ap);
	va_end(ap);

	return (ret < 0) ? NULL : status_buf;
}

const char *
status_fmt_human(uintmax_t num, int base)
{
	double      scaled;
	size_t      i, prefixlen;
	const char **prefix;
	const char *prefix_1000[] = { "", "k", "M", "G", "T", "P", "E", "Z", "Y" };
	const char *prefix_1024[] = { "", "Ki", "Mi", "Gi", "Ti", "Pi", "Ei", "Zi", "Yi" };

	switch (base) {
	case 1000:
		prefix = prefix_1000;
		prefixlen = STATUS_LEN(prefix_1000);
		break;
	case 1024:
		prefix = prefix_1024;
		prefixlen = STATUS_LEN(prefix_1024);
		break;
	default:
		status_warn("fmt_human invalid base");
		return NULL;
	}

	scaled = num;
	for (i = 0; i < prefixlen && scaled >= base; i++)
		scaled /= base;

	return status_bprintf("%.1f %s", scaled, prefix[i]);
}

int
status_pscanf(const char *path, const char *fmt, ...)
{
	FILE   *fp;
	va_list ap;
	int     n;

	if (!(fp = fopen(path, "r"))) {
		status_warn("fopen '%s': %s", path, strerror(errno));
		return -1;
	}
	va_start(ap, fmt);
	n = vfscanf(fp, fmt, ap);
	va_end(ap);
	fclose(fp);

	return (n == EOF) ? -1 : n;
}
