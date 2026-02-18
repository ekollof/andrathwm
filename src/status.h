/* AndrathWM - embedded status module (slstatus-based)
 * See LICENSE file for copyright and license details. */

#ifndef STATUS_H
#define STATUS_H

#include <glib.h>

/*
 * status_init - initialise the status bar timer.
 *
 * @ctx: the GMainContext to attach the timer source to.  Pass NULL to use
 *       the default (main-thread) context.
 *
 * Creates a GTimeout source and attaches it to @ctx so that status_resume()
 * is called automatically at each tick without the caller needing to watch
 * any file descriptor.
 *
 * Returns 0 on success, -1 on failure.
 */
int  status_init(GMainContext *ctx);
void status_cleanup(void);
void status_resume(void);

#endif /* STATUS_H */
