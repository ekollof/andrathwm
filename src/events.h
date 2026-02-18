/* AndrathWM - X event handlers
 * See LICENSE file for copyright and license details. */

#ifndef EVENTS_H
#define EVENTS_H

#include "awm.h"

/* X event handlers */
void buttonpress(XEvent *e);
void checkotherwm(void);
void clientmessage(XEvent *e);
void configurenotify(XEvent *e);
void configurerequest(XEvent *e);
void destroynotify(XEvent *e);
void enternotify(XEvent *e);
void expose(XEvent *e);
void focusin(XEvent *e);
void keypress(XEvent *e);
void mappingnotify(XEvent *e);
void maprequest(XEvent *e);
void motionnotify(XEvent *e);
void propertynotify(XEvent *e);
void resizerequest(XEvent *e);
void unmapnotify(XEvent *e);

/* signal / fake_signal */
int fake_signal(void);

/* key / button grab */
void grabkeys(void);
void updatenumlockmask(void);

/* X error handlers */
int xerror(Display *dpy, XErrorEvent *ee);
int xerrordummy(Display *dpy, XErrorEvent *ee);
int xerrorstart(Display *dpy, XErrorEvent *ee);

#endif /* EVENTS_H */
