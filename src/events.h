/* AndrathWM - X event handlers
 * See LICENSE file for copyright and license details. */

#ifndef EVENTS_H
#define EVENTS_H

#include "awm.h"

/* X event handlers */
void buttonpress(AwmEvent *e);
void checkotherwm(void);
void clientmessage(AwmEvent *e);
void configurenotify(AwmEvent *e);
void configurerequest(AwmEvent *e);
void destroynotify(AwmEvent *e);
void enternotify(AwmEvent *e);
void leavenotify(AwmEvent *e);
void expose(AwmEvent *e);
void focusin(AwmEvent *e);
void keypress(AwmEvent *e);
void keyrelease(AwmEvent *e);
void mappingnotify(AwmEvent *e);
void maprequest(AwmEvent *e);
void motionnotify(AwmEvent *e);
void propertynotify(AwmEvent *e);
void resizerequest(AwmEvent *e);
void unmapnotify(AwmEvent *e);

/* signal / fake_signal */
int fake_signal(void);

/* key / button grab */
void grabkeys(void);
void updatenumlockmask(void);

#endif /* EVENTS_H */
