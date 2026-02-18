/* AndrathWM - embedded status module (slstatus-based)
 * See LICENSE file for copyright and license details. */

#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifdef __linux__
#include <sys/timerfd.h>
#endif

#define MINICORO_IMPL
#include "minicoro.h"

#include "awm.h"
#include "status.h"
#include "status_components.h"
#include "status_util.h"
#include "status_config.h"

#define STATUS_COMPONENT_MAX 256

static mco_coro *status_coro = NULL;
static int       status_timer_fd = -1;
static time_t    last_update_time[STATUS_ARGS_LEN];
static char      cached_results[STATUS_ARGS_LEN][STATUS_COMPONENT_MAX];

static void
status_set_text(const char *text)
{
	size_t len;

	if (!text)
		return;
	len = strlen(text);
	if (len >= sizeof(stext))
		len = sizeof(stext) - 1;
	memcpy(stext, text, len);
	stext[len] = '\0';
	barsdirty = 1;
}

static void
status_prime_components(void)
{
	struct timespec ts = { .tv_sec = 0, .tv_nsec = 500000000 };
	size_t          i;
	const char     *res;
	time_t          current_time;

	memset(last_update_time, 0, sizeof(last_update_time));
	for (i = 0; i < STATUS_ARGS_LEN; i++) {
		strncpy(cached_results[i], status_unknown_str,
		    sizeof(cached_results[i]) - 1);
		cached_results[i][sizeof(cached_results[i]) - 1] = '\0';
	}

	for (i = 0; i < STATUS_ARGS_LEN; i++) {
		if (status_args[i].func == cpu_perc) {
			res = status_args[i].func(status_args[i].args);
			if (res) {
				strncpy(cached_results[i], res,
				    sizeof(cached_results[i]) - 1);
				cached_results[i][sizeof(cached_results[i]) - 1] = '\0';
			}
		}
	}

	nanosleep(&ts, NULL);

	current_time = time(NULL);
	for (i = 0; i < STATUS_ARGS_LEN; i++) {
		res = status_args[i].func(status_args[i].args);
		if (res) {
			strncpy(cached_results[i], res, sizeof(cached_results[i]) - 1);
			cached_results[i][sizeof(cached_results[i]) - 1] = '\0';
		}
		last_update_time[i] = current_time;
	}
}

static void
status_build(char *out, size_t out_len)
{
	size_t     i, len;
	time_t     current_time;
	const char *res;
	int        ret;

	if (!out || out_len == 0)
		return;

	out[0] = '\0';
	len = 0;
	current_time = time(NULL);

	for (i = 0; i < STATUS_ARGS_LEN; i++) {
		if (current_time - last_update_time[i] >= status_args[i].interval) {
			res = status_args[i].func(status_args[i].args);
			if (res) {
				strncpy(cached_results[i], res,
				    sizeof(cached_results[i]) - 1);
				cached_results[i][sizeof(cached_results[i]) - 1] = '\0';
			}
			last_update_time[i] = current_time;
		}

		res = cached_results[i];
		ret = status_esnprintf(status_buf, sizeof(status_buf), status_args[i].fmt,
		    res ? res : status_unknown_str);
		if (ret < 0)
			continue;
		if (len + (size_t) ret >= out_len)
			break;
		memcpy(out + len, status_buf, ret);
		len += (size_t) ret;
		out[len] = '\0';
	}
}

static void
status_coro_main(mco_coro *co)
{
	char text[STATUS_MAXLEN];

	while (1) {
		status_build(text, sizeof(text));
		status_set_text(text);
		mco_yield(co);
	}
}

int
status_init(int *out_timer_fd)
{
	mco_desc   desc;
	mco_result res;

	status_prime_components();

	desc = mco_desc_init(status_coro_main, 0);
	res = mco_create(&status_coro, &desc);
	if (res != MCO_SUCCESS) {
		awm_error("status coroutine init failed: %s",
		    mco_result_description(res));
		status_coro = NULL;
		return -1;
	}

#ifdef __linux__
	status_timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
	if (status_timer_fd < 0) {
		awm_error("timerfd_create failed: %s", strerror(errno));
		mco_destroy(status_coro);
		status_coro = NULL;
		return -1;
	}

	{
		struct itimerspec its;
		unsigned int     interval_ms = status_interval_ms ? status_interval_ms : 1000;

		its.it_interval.tv_sec = interval_ms / 1000;
		its.it_interval.tv_nsec = (interval_ms % 1000) * 1000000;
		its.it_value = its.it_interval;
		if (timerfd_settime(status_timer_fd, 0, &its, NULL) < 0) {
			awm_error("timerfd_settime failed: %s", strerror(errno));
			close(status_timer_fd);
			status_timer_fd = -1;
			mco_destroy(status_coro);
			status_coro = NULL;
			return -1;
		}
	}
#else
	awm_warn("status timerfd unsupported on this platform");
	status_timer_fd = -1;
#endif

	status_resume();
	if (out_timer_fd)
		*out_timer_fd = status_timer_fd;
	return 0;
}

void
status_cleanup(void)
{
	if (status_timer_fd >= 0) {
		close(status_timer_fd);
		status_timer_fd = -1;
	}
	if (status_coro) {
		mco_destroy(status_coro);
		status_coro = NULL;
	}
}

void
status_resume(void)
{
	mco_result res;

	if (!status_coro)
		return;
	if (mco_status(status_coro) == MCO_DEAD)
		return;

	res = mco_resume(status_coro);
	if (res != MCO_SUCCESS)
		awm_warn("status coroutine resume failed: %s",
		    mco_result_description(res));
}
