/* AndrathWM - client management
 * See LICENSE file for copyright and license details. */

#ifndef CLIENT_H
#define CLIENT_H

#include "awm.h"

/* client lifecycle */
void applyrules(Client *c);
int  applysizehints(Client *c, int *x, int *y, int *w, int *h, int interact);
void attach(Client *c);
void attachclients(Monitor *m);
void attachstack(Client *c);
void configure(Client *c);
void detach(Client *c);
void detachstack(Client *c);
void focus(Client *c);
void focusstack(const Arg *arg);
void focusstackhidden(const Arg *arg);
void focuswin(const Arg *arg);
void freeicon(Client *c);
xcb_atom_t getatomprop(Client *c, xcb_atom_t prop);
int        getrootptr(int *x, int *y);
long       getstate(xcb_window_t w);
int        gettextprop(
           xcb_window_t w, xcb_atom_t atom, char *text, unsigned int size);
cairo_surface_t *getwmicon(xcb_window_t w, int size);
void             grabbuttons(Client *c, int focused);
void             hide(Client *c);
void             hidewin(const Arg *arg);
void             incnmaster(const Arg *arg);
void             killclient(const Arg *arg);
void             manage(xcb_window_t w, xcb_get_geometry_reply_t *gr);
void             movemouse(const Arg *arg);
Client          *nexttiled(Client *c, Monitor *m);
void             pop(Client *c);
void             resize(Client *c, int x, int y, int w, int h, int interact);
void             resizeclient(Client *c, int x, int y, int w, int h);
void             resizemouse(const Arg *arg);
void             restorewin(const Arg *arg);
void             sendmon(Client *c, Monitor *m);
void             setclientstate(Client *c, long state);
void             setfullscreen(Client *c, int fullscreen);
void             setgaps(const Arg *arg);
void             setlayout(const Arg *arg);
void             setmfact(const Arg *arg);
void             seturgent(Client *c, int urg);
void             showall(const Arg *arg);
void             showhide(Client *c);
void             show(Client *c);
void             tag(const Arg *arg);
void             tagmon(const Arg *arg);
void             togglefloating(const Arg *arg);
void             togglescratch(const Arg *arg);
void             toggletag(const Arg *arg);
void             toggleview(const Arg *arg);
void             unfocus(Client *c, int setfocus);
void             unmanage(Client *c, int destroyed);
void             updatesizehints(Client *c);
void             updatetitle(Client *c);
void             updatewindowtype(Client *c);
void             updatewmhints(Client *c);
void             view(const Arg *arg);
void             warp(const Client *c);
Client          *wintoclient(xcb_window_t w);
void             zoom(const Arg *arg);
void             movestack(const Arg *arg);

#endif /* CLIENT_H */
