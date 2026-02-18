/* AndrathWM - XEMBED systray functions
 * See LICENSE file for copyright and license details. */

#ifndef SYSTRAY_H
#define SYSTRAY_H

#include "awm.h"

unsigned int getsystraywidth(void);
void         removesystrayicon(Client *i);
void         updatesystray(void);
void         updatesystrayiconcolors(void);
void         updatesystrayicongeom(Client *i, int w, int h);
void         updatesystrayiconstate(Client *i, XPropertyEvent *ev);
Client      *wintosystrayicon(Window w);
void         addsniiconsystray(Window w, int width, int height);
void         removesniiconsystray(Window w);

#endif /* SYSTRAY_H */
