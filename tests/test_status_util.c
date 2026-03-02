/* See LICENSE file for copyright and license details. */
/* Tests for src/status_util.c: status_fmt_human(), status_esnprintf(). */

#include <stdint.h>
#include <string.h>

#include "minunit.h"
#include "../src/status_util.h"

/* -------------------------------------------------------------------------
 * status_fmt_human — base 1000
 * ---------------------------------------------------------------------- */

MU_SUITE(test_fmt_human_1000_zero)
{
	const char *r = status_fmt_human(0, 1000);
	MU_CHECK("0 formats as '0.0 '", r && strcmp(r, "0.0 ") == 0);
}

MU_SUITE(test_fmt_human_1000_below_threshold)
{
	const char *r = status_fmt_human(999, 1000);
	MU_CHECK("999 formats as '999.0 '", r && strcmp(r, "999.0 ") == 0);
}

MU_SUITE(test_fmt_human_1000_exact_kilo)
{
	const char *r = status_fmt_human(1000, 1000);
	MU_CHECK("1000 formats as '1.0 k'", r && strcmp(r, "1.0 k") == 0);
}

MU_SUITE(test_fmt_human_1000_mega)
{
	const char *r = status_fmt_human(1500000, 1000);
	MU_CHECK("1500000 formats as '1.5 M'", r && strcmp(r, "1.5 M") == 0);
}

MU_SUITE(test_fmt_human_1000_giga)
{
	const char *r = status_fmt_human(2000000000ULL, 1000);
	MU_CHECK("2e9 formats as '2.0 G'", r && strcmp(r, "2.0 G") == 0);
}

/* -------------------------------------------------------------------------
 * status_fmt_human — base 1024
 * ---------------------------------------------------------------------- */

MU_SUITE(test_fmt_human_1024_zero)
{
	const char *r = status_fmt_human(0, 1024);
	MU_CHECK("0 base-1024 formats as '0.0 '", r && strcmp(r, "0.0 ") == 0);
}

MU_SUITE(test_fmt_human_1024_below_threshold)
{
	const char *r = status_fmt_human(1023, 1024);
	MU_CHECK("1023 formats as '1023.0 '", r && strcmp(r, "1023.0 ") == 0);
}

MU_SUITE(test_fmt_human_1024_exact_kibi)
{
	const char *r = status_fmt_human(1024, 1024);
	MU_CHECK("1024 formats as '1.0 Ki'", r && strcmp(r, "1.0 Ki") == 0);
}

MU_SUITE(test_fmt_human_1024_mebi)
{
	const char *r = status_fmt_human(1024ULL * 1024, 1024);
	MU_CHECK("1 MiB formats as '1.0 Mi'", r && strcmp(r, "1.0 Mi") == 0);
}

MU_SUITE(test_fmt_human_1024_gibi)
{
	const char *r = status_fmt_human(1024ULL * 1024 * 1024, 1024);
	MU_CHECK("1 GiB formats as '1.0 Gi'", r && strcmp(r, "1.0 Gi") == 0);
}

/* -------------------------------------------------------------------------
 * status_fmt_human — invalid base
 * ---------------------------------------------------------------------- */

MU_SUITE(test_fmt_human_invalid_base)
{
	const char *r = status_fmt_human(1000, 512);
	MU_CHECK("invalid base returns NULL", r == NULL);
}

/* -------------------------------------------------------------------------
 * status_esnprintf
 * ---------------------------------------------------------------------- */

MU_SUITE(test_esnprintf_normal)
{
	char buf[32];
	int  ret = status_esnprintf(buf, sizeof(buf), "hello %d", 42);
	MU_CHECK("normal write succeeds (ret >= 0)", ret >= 0);
	MU_CHECK("normal write content correct", strcmp(buf, "hello 42") == 0);
}

MU_SUITE(test_esnprintf_exact_fit)
{
	char buf[9]; /* "hello 42" = 8 chars + NUL */
	int  ret = status_esnprintf(buf, sizeof(buf), "hello %d", 42);
	MU_CHECK("exact-fit write succeeds", ret >= 0);
	MU_CHECK("exact-fit content correct", strcmp(buf, "hello 42") == 0);
}

MU_SUITE(test_esnprintf_truncation)
{
	char buf[4]; /* too small for "hello 42" */
	int  ret = status_esnprintf(buf, sizeof(buf), "hello %d", 42);
	MU_CHECK("truncation returns -1", ret == -1);
}

/* -------------------------------------------------------------------------
 * main
 * ---------------------------------------------------------------------- */

int
main(void)
{
	MU_RUN(test_fmt_human_1000_zero);
	MU_RUN(test_fmt_human_1000_below_threshold);
	MU_RUN(test_fmt_human_1000_exact_kilo);
	MU_RUN(test_fmt_human_1000_mega);
	MU_RUN(test_fmt_human_1000_giga);
	MU_RUN(test_fmt_human_1024_zero);
	MU_RUN(test_fmt_human_1024_below_threshold);
	MU_RUN(test_fmt_human_1024_exact_kibi);
	MU_RUN(test_fmt_human_1024_mebi);
	MU_RUN(test_fmt_human_1024_gibi);
	MU_RUN(test_fmt_human_invalid_base);
	MU_RUN(test_esnprintf_normal);
	MU_RUN(test_esnprintf_exact_fit);
	MU_RUN(test_esnprintf_truncation);

	MU_REPORT();
	return MU_EXIT();
}
