/* AndrathWM - embedded status module (slstatus-based)
 * See LICENSE file for copyright and license details. */

#include <string.h>
#include <time.h>

#include <glib.h>

#include "awm.h"
#include "monitor.h"
#include "status.h"
#include "status_components.h"
#include "status_config.h"
#include "status_util.h"
#include "systray.h"

#define STATUS_COMPONENT_MAX 256

static guint  status_timer_id = 0;
static time_t last_update_time[STATUS_ARGS_LEN];
static char   cached_results[STATUS_ARGS_LEN][STATUS_COMPONENT_MAX];

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
	barsdirty  = 1;
}

static void
status_prime_components(void)
{
	size_t i;

	memset(last_update_time, 0, sizeof(last_update_time));
	for (i = 0; i < STATUS_ARGS_LEN; i++) {
		strncpy(cached_results[i], status_unknown_str,
		    sizeof(cached_results[i]) - 1);
		cached_results[i][sizeof(cached_results[i]) - 1] = '\0';
	}

	/* Prime components that require an initial call to seed their state
	 * (e.g. cpu_perc needs a /proc/stat snapshot before the first delta
	 * can be computed).  Using the explicit prime flag avoids fragile
	 * function-pointer comparisons that break under LTO. */
	for (i = 0; i < STATUS_ARGS_LEN; i++) {
		if (status_args[i].prime)
			(void) status_args[i].func(status_args[i].args);
	}
}

static void
status_build(char *out, size_t out_len)
{
	size_t      i, len;
	time_t      current_time;
	const char *res;
	int         ret;

	if (!out || out_len == 0)
		return;

	out[0]       = '\0';
	len          = 0;
	current_time = time(NULL);

	for (i = 0; i < STATUS_ARGS_LEN; i++) {
		if (current_time - last_update_time[i] >= status_args[i].interval) {
			res = status_args[i].func(status_args[i].args);
			if (res) {
				strncpy(cached_results[i], res, sizeof(cached_results[i]) - 1);
				cached_results[i][sizeof(cached_results[i]) - 1] = '\0';
			}
			last_update_time[i] = current_time;
		}

		res = cached_results[i];
		ret = status_esnprintf(status_buf, sizeof(status_buf),
		    status_args[i].fmt, res ? res : status_unknown_str);
		if (ret < 0)
			continue;
		if (len + (size_t) ret >= out_len)
			break;
		memcpy(out + len, status_buf, ret);
		len += (size_t) ret;
		out[len] = '\0';
	}
}

/* GLib timer callback — fires at each status_interval_ms tick. */
static gboolean
status_timer_cb(gpointer user_data)
{
	(void) user_data;
	status_resume();
	/* barsdirty was set by status_set_text; flush immediately since there
	 * may be no pending X events to trigger x_dispatch_cb. */
	if (barsdirty) {
		drawbars();
		updatesystray();
		barsdirty = 0;
	}
	return G_SOURCE_CONTINUE;
}

void
status_init(GMainContext *ctx)
{
	unsigned int interval_ms = status_interval_ms ? status_interval_ms : 1000;

	status_prime_components();

	/* Attach the repeating timer to the provided context.
	 * g_source_attach() requires we create the source manually so we can
	 * target a specific context rather than always the default one. */
	{
		GSource *src = g_timeout_source_new(interval_ms);
		g_source_set_callback(src, status_timer_cb, NULL, NULL);
		g_source_set_priority(src, G_PRIORITY_DEFAULT);
		status_timer_id = g_source_attach(src, ctx);
		g_source_unref(src);
	}

	/* Fire once immediately so the bar shows data before the first tick. */
	status_resume();
}

void
status_cleanup(void)
{
	if (status_timer_id > 0) {
		g_source_remove(status_timer_id);
		status_timer_id = 0;
	}
}

void
status_resume(void)
{
	char text[STATUS_MAXLEN];

	status_build(text, sizeof(text));
	status_set_text(text);
}
