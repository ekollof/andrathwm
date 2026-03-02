/* See LICENSE file for copyright and license details. */
/* Minimal unit test harness — no external dependencies. */
#ifndef MINUNIT_H
#define MINUNIT_H

#include <stdio.h>

static int mu_tests_run    = 0;
static int mu_tests_passed = 0;
static int mu_tests_failed = 0;

#define MU_CHECK(msg, test)                                                \
	do {                                                                   \
		mu_tests_run++;                                                    \
		if (!(test)) {                                                     \
			fprintf(stderr, "FAIL  %s:%d: %s\n", __FILE__, __LINE__, msg); \
			mu_tests_failed++;                                             \
		} else {                                                           \
			mu_tests_passed++;                                             \
		}                                                                  \
	} while (0)

#define MU_SUITE(name) static void name(void)

#define MU_RUN(suite) \
	do {              \
		suite();      \
	} while (0)

#define MU_REPORT()                                                   \
	do {                                                              \
		printf("Tests: %d run, %d passed, %d failed\n", mu_tests_run, \
		    mu_tests_passed, mu_tests_failed);                        \
	} while (0)

#define MU_EXIT() (mu_tests_failed ? 1 : 0)

#endif /* MINUNIT_H */
