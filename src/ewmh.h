/* AndrathWM - EWMH/ICCCM protocol functions
 * See LICENSE file for copyright and license details. */

#ifndef EWMH_H
#define EWMH_H

#include "awm.h"

/* EWMH property setters */
void setcurrentdesktop(void);
void setdesktopnames(void);
void setnumdesktops(void);
void setviewport(void);
void setfocus(Client *c);
void setwmstate(Client *c);
void setewmhdesktop(Client *c);
void updateclientlist(void);
void updatecurrentdesktop(void);
void updateworkarea(Monitor *m);

/* ICCCM helpers */
int sendevent(Window w, Atom proto, int mask, long d0, long d1, long d2,
    long d3, long d4);
unsigned long getembedinfo(Client *c);

#endif /* EWMH_H */
