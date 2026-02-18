/* See LICENSE file for copyright and license details.
 *
 * Generic event queue/task scheduler
 * Provides deferred task execution integrated with event loop
 */

#include <stdlib.h>

#include "log.h"
#include "queue.h"
#include "util.h"

/* Task node in queue */
typedef struct Task {
	queue_callback_fn callback;
	void             *data;
	struct Task      *next;
} Task;

/* Priority queues - separate list for each priority level */
static Task *queue_heads[QUEUE_PRIORITY_COUNT]  = { NULL };
static Task *queue_tails[QUEUE_PRIORITY_COUNT]  = { NULL };
static int   queue_counts[QUEUE_PRIORITY_COUNT] = { 0 };
static int   queue_total_count                  = 0;

static void
queue_drop_lowest(void)
{
	int   priority;
	Task *task;

	for (priority = 0; priority < QUEUE_PRIORITY_COUNT; priority++) {
		if (queue_heads[priority]) {
			task                  = queue_heads[priority];
			queue_heads[priority] = task->next;
			if (!queue_heads[priority])
				queue_tails[priority] = NULL;
			queue_counts[priority]--;
			queue_total_count--;
			free(task);
			return;
		}
	}
}

void
queue_init(void)
{
	int i;

	for (i = 0; i < QUEUE_PRIORITY_COUNT; i++) {
		queue_heads[i]  = NULL;
		queue_tails[i]  = NULL;
		queue_counts[i] = 0;
	}
	queue_total_count = 0;
}

void
queue_cleanup(void)
{
	Task *task, *next;
	int   i;

	for (i = 0; i < QUEUE_PRIORITY_COUNT; i++) {
		task = queue_heads[i];
		while (task) {
			next = task->next;
			free(task);
			task = next;
		}
		queue_heads[i]  = NULL;
		queue_tails[i]  = NULL;
		queue_counts[i] = 0;
	}
	queue_total_count = 0;
}

void
queue_add(queue_callback_fn callback, void *data, int priority)
{
	Task *task;

	if (!callback) {
		awm_error("NULL callback");
		return;
	}

	if (priority < 0 || priority >= QUEUE_PRIORITY_COUNT) {
		awm_error("Invalid priority %d, using NORMAL", priority);
		priority = QUEUE_PRIORITY_NORMAL;
	}

	while (queue_total_count >= QUEUE_MAX_DEPTH) {
		queue_drop_lowest();
	}

	task = calloc(1, sizeof(Task));
	if (!task) {
		awm_error("Task allocation failed");
		return;
	}

	task->callback = callback;
	task->data     = data;
	task->next     = NULL;

	if (queue_tails[priority]) {
		queue_tails[priority]->next = task;
		queue_tails[priority]       = task;
	} else {
		queue_heads[priority] = task;
		queue_tails[priority] = task;
	}

	queue_counts[priority]++;
	queue_total_count++;
}

int
queue_process(int limit)
{
	Task *task;
	int   processed = 0;
	int   priority;

	for (priority = QUEUE_PRIORITY_COUNT - 1; priority >= 0; priority--) {
		while (queue_heads[priority] && (limit == 0 || processed < limit)) {
			task                  = queue_heads[priority];
			queue_heads[priority] = task->next;

			if (!queue_heads[priority])
				queue_tails[priority] = NULL;

			queue_counts[priority]--;
			queue_total_count--;

			if (task->callback)
				task->callback(task->data);

			free(task);
			processed++;
		}

		if (limit > 0 && processed >= limit)
			break;
	}

	return processed;
}

int
queue_depth(void)
{
	return queue_total_count;
}

int
queue_depth_priority(int priority)
{
	if (priority < 0 || priority >= QUEUE_PRIORITY_COUNT)
		return 0;

	return queue_counts[priority];
}
