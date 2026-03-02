/* See LICENSE file for copyright and license details. */
/* Tests for src/status_util.c: status_fmt_human(), status_esnprintf(). */

#include <stdint.h>
#include <string.h>

#include "greatest.h"
#include "../src/status_util.h"

/* -------------------------------------------------------------------------
 * status_fmt_human — base 1000
 * ---------------------------------------------------------------------- */

TEST
fmt_human_1000_zero(void)
{
	ASSERT_STR_EQ("0.0 ", status_fmt_human(0, 1000));
	PASS();
}

TEST
fmt_human_1000_below_threshold(void)
{
	ASSERT_STR_EQ("999.0 ", status_fmt_human(999, 1000));
	PASS();
}

TEST
fmt_human_1000_exact_kilo(void)
{
	ASSERT_STR_EQ("1.0 k", status_fmt_human(1000, 1000));
	PASS();
}

TEST
fmt_human_1000_mega(void)
{
	ASSERT_STR_EQ("1.5 M", status_fmt_human(1500000, 1000));
	PASS();
}

TEST
fmt_human_1000_giga(void)
{
	ASSERT_STR_EQ("2.0 G", status_fmt_human(2000000000ULL, 1000));
	PASS();
}

/* -------------------------------------------------------------------------
 * status_fmt_human — base 1024
 * ---------------------------------------------------------------------- */

TEST
fmt_human_1024_zero(void)
{
	ASSERT_STR_EQ("0.0 ", status_fmt_human(0, 1024));
	PASS();
}

TEST
fmt_human_1024_below_threshold(void)
{
	ASSERT_STR_EQ("1023.0 ", status_fmt_human(1023, 1024));
	PASS();
}

TEST
fmt_human_1024_exact_kibi(void)
{
	ASSERT_STR_EQ("1.0 Ki", status_fmt_human(1024, 1024));
	PASS();
}

TEST
fmt_human_1024_mebi(void)
{
	ASSERT_STR_EQ("1.0 Mi", status_fmt_human(1024ULL * 1024, 1024));
	PASS();
}

TEST
fmt_human_1024_gibi(void)
{
	ASSERT_STR_EQ("1.0 Gi", status_fmt_human(1024ULL * 1024 * 1024, 1024));
	PASS();
}

/* -------------------------------------------------------------------------
 * status_fmt_human — invalid base
 * ---------------------------------------------------------------------- */

TEST
fmt_human_invalid_base(void)
{
	ASSERT_EQ(NULL, status_fmt_human(1000, 512));
	PASS();
}

/* -------------------------------------------------------------------------
 * status_esnprintf
 * ---------------------------------------------------------------------- */

TEST
esnprintf_normal(void)
{
	char buf[32];
	int  ret = status_esnprintf(buf, sizeof(buf), "hello %d", 42);
	ASSERT(ret >= 0);
	ASSERT_STR_EQ("hello 42", buf);
	PASS();
}

TEST
esnprintf_exact_fit(void)
{
	char buf[9]; /* "hello 42" = 8 chars + NUL */
	int  ret = status_esnprintf(buf, sizeof(buf), "hello %d", 42);
	ASSERT(ret >= 0);
	ASSERT_STR_EQ("hello 42", buf);
	PASS();
}

TEST
esnprintf_truncation(void)
{
	char buf[4]; /* too small for "hello 42" */
	int  ret = status_esnprintf(buf, sizeof(buf), "hello %d", 42);
	ASSERT_EQ(-1, ret);
	PASS();
}

/* -------------------------------------------------------------------------
 * Suites
 * ---------------------------------------------------------------------- */

SUITE(suite_fmt_human)
{
	RUN_TEST(fmt_human_1000_zero);
	RUN_TEST(fmt_human_1000_below_threshold);
	RUN_TEST(fmt_human_1000_exact_kilo);
	RUN_TEST(fmt_human_1000_mega);
	RUN_TEST(fmt_human_1000_giga);
	RUN_TEST(fmt_human_1024_zero);
	RUN_TEST(fmt_human_1024_below_threshold);
	RUN_TEST(fmt_human_1024_exact_kibi);
	RUN_TEST(fmt_human_1024_mebi);
	RUN_TEST(fmt_human_1024_gibi);
	RUN_TEST(fmt_human_invalid_base);
}

SUITE(suite_esnprintf)
{
	RUN_TEST(esnprintf_normal);
	RUN_TEST(esnprintf_exact_fit);
	RUN_TEST(esnprintf_truncation);
}

/* -------------------------------------------------------------------------
 * main
 * ---------------------------------------------------------------------- */

GREATEST_MAIN_DEFS();

int
main(int argc, char **argv)
{
	GREATEST_MAIN_BEGIN();
	RUN_SUITE(suite_fmt_human);
	RUN_SUITE(suite_esnprintf);
	GREATEST_MAIN_END();
}
