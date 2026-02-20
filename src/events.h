/* AndrathWM - X event handlers
 * See LICENSE file for copyright and license details. */

#ifndef EVENTS_H
#define EVENTS_H

#include "awm.h"

/* X event handlers */
void buttonpress(xcb_generic_event_t *e);
void checkotherwm(void);
void clientmessage(xcb_generic_event_t *e);
void configurenotify(xcb_generic_event_t *e);
void configurerequest(xcb_generic_event_t *e);
void destroynotify(xcb_generic_event_t *e);
void enternotify(xcb_generic_event_t *e);
void expose(xcb_generic_event_t *e);
void focusin(xcb_generic_event_t *e);
void keypress(xcb_generic_event_t *e);
void mappingnotify(xcb_generic_event_t *e);
void maprequest(xcb_generic_event_t *e);
void motionnotify(xcb_generic_event_t *e);
void propertynotify(xcb_generic_event_t *e);
void resizerequest(xcb_generic_event_t *e);
void unmapnotify(xcb_generic_event_t *e);

/* signal / fake_signal */
int fake_signal(void);

/* key / button grab */
void grabkeys(void);
void updatenumlockmask(void);

/* XCB async error handler — called from x_dispatch_cb() on response_type==0 */
int xcb_error_handler(xcb_generic_error_t *e);

#endif /* EVENTS_H */
