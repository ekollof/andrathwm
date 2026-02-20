/* AndrathWM - monitor management
 * See LICENSE file for copyright and license details. */

#ifndef MONITOR_H
#define MONITOR_H

#include "awm.h"

/* monitor lifecycle */
Monitor *createmon(void);
void     cleanupmon(Monitor *mon);

/* layout / arrangement */
void arrange(Monitor *m);
void arrangemon(Monitor *m);
void monocle(Monitor *m);
void tile(Monitor *m);
void restack(Monitor *m);

/* bar */
void drawbar(Monitor *m);
void drawbars(void);
void togglebar(const Arg *arg);
void updatebars(void);
void updatebarpos(Monitor *m);
void resizebarwin(Monitor *m);

/* monitor queries */
Monitor *dirtomon(int dir);
Monitor *recttomon(int x, int y, int w, int h);
Monitor *wintomon(xcb_window_t w);
Monitor *systraytomon(Monitor *m);

/* focus */
void focusmon(const Arg *arg);

/* geometry */
int  updategeom(void);
void updatestatus(void);

#endif /* MONITOR_H */
