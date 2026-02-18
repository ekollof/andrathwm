/* AndrathWM - embedded status module (slstatus-based)
 * See LICENSE file for copyright and license details. */

#ifndef STATUS_H
#define STATUS_H

int  status_init(int *out_timer_fd);
void status_cleanup(void);
void status_resume(void);

#endif /* STATUS_H */
