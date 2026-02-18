/* See LICENSE file for copyright and license details.
 *
 * Generic event queue/task scheduler
 * Provides deferred task execution integrated with event loop
 */

#ifndef QUEUE_H
#define QUEUE_H

/* Task priorities */
enum {
	QUEUE_PRIORITY_LOW    = 0,
	QUEUE_PRIORITY_NORMAL = 1,
	QUEUE_PRIORITY_HIGH   = 2,
	QUEUE_PRIORITY_COUNT  = 3
};

/* Maximum queue depth before dropping low-priority tasks */
#define QUEUE_MAX_DEPTH 1024

/* Task callback signature */
typedef void (*queue_callback_fn)(void *data);

/* Initialize queue system - call once at startup */
void queue_init(void);

/* Cleanup queue system - call once at shutdown */
void queue_cleanup(void);

/* Enqueue a task to run later
 * callback: function to call when task executes
 * data: user data passed to callback (ownership transferred to queue)
 * priority: QUEUE_PRIORITY_* constant
 */
void queue_add(queue_callback_fn callback, void *data, int priority);

/* Process pending tasks from queue
 * limit: max tasks to process (0 = process all pending)
 * Returns: number of tasks processed
 * Call this from your event loop each iteration
 */
int queue_process(int limit);

/* Get number of pending tasks */
int queue_depth(void);

/* Get number of pending tasks by priority */
int queue_depth_priority(int priority);

#endif /* QUEUE_H */
