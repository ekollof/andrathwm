/* AndrathWM - XEMBED systray functions
 * See LICENSE file for copyright and license details. */

#ifndef SYSTRAY_H
#define SYSTRAY_H

#ifdef BACKEND_X11

#include "awm.h"

unsigned long clr_to_argb(Clr *clr);
unsigned int  getsystraywidth(void);
void          removesystrayicon(Client *i);
void          updatesystray(void);
void          updatesystrayiconcolors(void);
void          updatesystrayicongeom(Client *i, int w, int h);
void          updatesystrayiconstate(Client *i, AtomId atom);
Client       *wintosystrayicon(WinId w);
void          addsniiconsystray(WinId w, int width, int height);
void          removesniiconsystray(WinId w);

#endif /* BACKEND_X11 */

#endif /* SYSTRAY_H */
