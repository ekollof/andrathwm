/* AndrathWM - shared types, macros, enums and extern globals
 * See LICENSE file for copyright and license details. */

#ifndef AWM_H
#define AWM_H

#include <X11/X.h>
#include <X11/Xatom.h>
#include <X11/Xft/Xft.h>
#include <X11/Xlib.h>
#include <X11/Xproto.h>
#include <X11/Xresource.h>
#include <X11/Xutil.h>
#include <X11/cursorfont.h>
#include <X11/keysym.h>
#include <cairo/cairo.h>
#include <errno.h>
#include <locale.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <syslog.h>
#include <unistd.h>
#ifdef XINERAMA
#include <X11/extensions/Xinerama.h>
#endif /* XINERAMA */
#ifdef XRANDR
#include <X11/extensions/Xrandr.h>
#endif /* XRANDR */
#ifdef XSS
#include <X11/extensions/scrnsaver.h>
#endif /* XSS */
#ifdef STATUSNOTIFIER
#include "sni.h"
#endif

#include "drw.h"
#include "icon.h"
#include "log.h"
#include "queue.h"
#include "util.h"

/* macros */
#define BUTTONMASK (ButtonPressMask | ButtonReleaseMask)
#define CLEANMASK(mask)                                             \
	(mask & ~(numlockmask | LockMask) &                             \
	    (ShiftMask | ControlMask | Mod1Mask | Mod2Mask | Mod3Mask | \
	        Mod4Mask | Mod5Mask))
#define INTERSECT(x, y, w, h, m)                                     \
	(MAX(0, MIN((x) + (w), (m)->wx + (m)->ww) - MAX((x), (m)->wx)) * \
	    MAX(0, MIN((y) + (h), (m)->wy + (m)->wh) - MAX((y), (m)->wy)))
#define ISVISIBLE(C, M) ((C->tags & M->tagset[M->seltags]))
#define LENGTH(X) (sizeof X / sizeof X[0])
#define MOUSEMASK (BUTTONMASK | PointerMotionMask)
#define WIDTH(X) ((X)->w + 2 * (X)->bw)
#define HEIGHT(X) ((X)->h + 2 * (X)->bw)
#define TAGMASK ((1 << LENGTH(tags)) - 1)
#define TAGSLENGTH (LENGTH(tags))
#define TEXTW(X) (drw_fontset_getwidth(drw, (X)) + lrpad)

#define GAP_TOGGLE 100
#define GAP_RESET 0

/* XEMBED / systray defines */
#define SYSTEM_TRAY_REQUEST_DOCK 0
#define XEMBED_EMBEDDED_NOTIFY 0
#define XEMBED_WINDOW_ACTIVATE 1
#define XEMBED_WINDOW_DEACTIVATE 2
#define XEMBED_REQUEST_FOCUS 3
#define XEMBED_FOCUS_IN 4
#define XEMBED_FOCUS_OUT 5
#define XEMBED_FOCUS_NEXT 6
#define XEMBED_FOCUS_PREV 7
#define XEMBED_MODALITY_ON 10
#define XEMBED_MODALITY_OFF 11
#define XEMBED_REGISTER_ACCELERATOR 12
#define XEMBED_UNREGISTER_ACCELERATOR 13
#define XEMBED_ACTIVATE_ACCELERATOR 14
#define XEMBED_FOCUS_CURRENT 0
#define XEMBED_FOCUS_FIRST 1
#define XEMBED_FOCUS_LAST 2
#define XEMBED_MAPPED (1 << 0)
#define XEMBED_VERSION 0

/* XRDB color loading helper */
#define XRDB_LOAD_COLOR(R, V)                                    \
	if (XrmGetResource(xrdb, R, NULL, &type, &value) == True) {  \
		if (value.addr != NULL && strnlen(value.addr, 8) == 7 && \
		    value.addr[0] == '#') {                              \
			int i = 1;                                           \
			for (; i <= 6; i++) {                                \
				if (value.addr[i] < 48)                          \
					break;                                       \
				if (value.addr[i] > 57 && value.addr[i] < 65)    \
					break;                                       \
				if (value.addr[i] > 70 && value.addr[i] < 97)    \
					break;                                       \
				if (value.addr[i] > 102)                         \
					break;                                       \
			}                                                    \
			if (i == 7) {                                        \
				strncpy(V, value.addr, 7);                       \
				V[7] = '\0';                                     \
			}                                                    \
		}                                                        \
	}

/* enums */
enum { CurNormal, CurResize, CurMove, CurLast }; /* cursor */
enum { SchemeNorm, SchemeSel };                  /* color schemes */
enum {
	NetSupported,
	NetWMName,
	NetWMIcon,
	NetWMState,
	NetWMCheck,
	NetSystemTray,
	NetSystemTrayOP,
	NetSystemTrayOrientation,
	NetSystemTrayOrientationHorz,
	NetSystemTrayColors,
	NetSystemTrayVisual,
	NetWMFullscreen,
	NetWMStateDemandsAttention,
	NetWMStateSticky,
	NetWMStateAbove,
	NetWMStateBelow,
	NetWMStateHidden,
	NetActiveWindow,
	NetWMWindowType,
	NetWMWindowTypeDialog,
	NetClientList,
	NetClientListStacking,
	NetWMDesktop,
	NetWMPid,
	NetDesktopNames,
	NetDesktopViewport,
	NetNumberOfDesktops,
	NetCurrentDesktop,
	NetWorkarea,
	NetCloseWindow,
	NetMoveResizeWindow,
	NetFrameExtents,
	NetLast
}; /* EWMH atoms */
enum { Manager, Xembed, XembedInfo, XLast }; /* Xembed atoms */
enum {
	WMProtocols,
	WMDelete,
	WMState,
	WMTakeFocus,
	WMLast
}; /* default atoms */
enum {
	ClkTagBar,
	ClkLtSymbol,
	ClkStatusText,
	ClkWinTitle,
	ClkClientWin,
	ClkRootWin,
	ClkLast
}; /* clicks */

/* types */
typedef union {
	int          i;
	unsigned int ui;
	float        f;
	const void  *v;
} Arg;

typedef struct {
	unsigned int click;
	unsigned int mask;
	unsigned int button;
	void (*func)(const Arg *arg);
	const Arg arg;
} Button;

typedef struct Clientlist Clientlist;
typedef struct Monitor    Monitor;
typedef struct Client     Client;
struct Client {
	char             name[256];
	cairo_surface_t *icon;
	float            mina, maxa;
	int              x, y, w, h;
	int              oldx, oldy, oldw, oldh;
	int          basew, baseh, incw, inch, maxw, maxh, minw, minh, hintsvalid;
	int          bw, oldbw;
	unsigned int tags;
	int isfixed, iscentered, isfloating, isurgent, neverfocus, oldstate,
	    isfullscreen;
	int      ishidden;
	int      issteam;
	int      issni;
	char     scratchkey;
	Client  *next;
	Client  *snext;
	Monitor *mon;
	Window   win;
};

typedef struct {
	unsigned int mod;
	KeySym       keysym;
	void (*func)(const Arg *);
	const Arg arg;
} Key;

typedef struct {
	unsigned int signum;
	void (*func)(const Arg *);
	const Arg arg;
} Signal;

typedef struct {
	const char *symbol;
	void (*arrange)(Monitor *);
} Layout;

typedef struct Pertag Pertag;
struct Monitor {
	char          ltsymbol[16];
	float         mfact;
	int           nmaster;
	int           num;
	int           by;
	int           mx, my, mw, mh;
	int           wx, wy, ww, wh;
	unsigned int  seltags;
	unsigned int  sellt;
	unsigned int  tagset[2];
	int           showbar;
	int           topbar;
	Clientlist   *cl;
	Client       *sel;
	Client       *stack;
	Monitor      *next;
	Window        barwin;
	const Layout *lt[2];
	Pertag       *pertag;
};

typedef struct {
	const char *class;
	const char  *instance;
	const char  *title;
	unsigned int tags;
	int          iscentered;
	int          isfloating;
	int          monitor;
	const char   scratchkey;
} Rule;

struct Clientlist {
	Client *clients;
	Client *stack;
};

typedef struct Systray Systray;
struct Systray {
	Window  win;
	Client *icons;
};

/* extern globals — defined in awm.c */
extern Display     *dpy;
extern Window       root, wmcheckwin;
extern int          screen;
extern int          sw, sh;
extern int          bh;
extern int          lrpad;
extern Drw         *drw;
extern Clr        **scheme;
extern Cur         *cursor[CurLast];
extern Monitor     *mons, *selmon;
extern Clientlist  *cl;
extern Systray     *systray;
extern Atom         wmatom[WMLast], netatom[NetLast], xatom[XLast];
extern char         stext[256];
extern int          running;
extern int          restart;
extern int          barsdirty;
extern unsigned int numlockmask;
extern int (*xerrorxlib)(Display *, XErrorEvent *);
/* config-derived globals referenced by dbus.c / icon.c */
extern const unsigned int sniconsize;
extern const unsigned int iconcachesize;
extern const unsigned int iconcachemaxentries;
extern const unsigned int dbustimeout;
#ifdef XRANDR
extern int randrbase, rrerrbase;
#endif
extern void (*handler[LASTEvent])(XEvent *);

/* core WM functions (defined in awm.c) */
void quit(const Arg *arg);

/* The Pertag struct uses LENGTH(tags) so must be defined after config.h.
 * It is defined in awm.c after the #include "config.h". */

#endif /* AWM_H */
