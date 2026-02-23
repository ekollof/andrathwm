/* AndrathWM - XEMBED systray functions
 * See LICENSE file for copyright and license details. */

#ifndef SYSTRAY_H
#define SYSTRAY_H

#include "awm.h"

unsigned long clr_to_argb(Clr *clr);
unsigned int  getsystraywidth(void);
void          removesystrayicon(Client *i);
void          updatesystray(void);
void          updatesystrayiconcolors(void);
void          updatesystrayicongeom(Client *i, int w, int h);
void    updatesystrayiconstate(Client *i, xcb_property_notify_event_t *ev);
Client *wintosystrayicon(xcb_window_t w);
void    addsniiconsystray(xcb_window_t w, int width, int height);
void    removesniiconsystray(xcb_window_t w);

#endif /* SYSTRAY_H */
