/* AndrathWM - shared types, macros, enums and extern globals
 * See LICENSE file for copyright and license details. */

#ifndef AWM_H
#define AWM_H

#include "x11_constants.h"
#include <xkbcommon/xkbcommon-keysyms.h>
#include <xcb/randr.h>
#include <xcb/xcb_icccm.h>
#include <xcb/xcb_keysyms.h>
#include <cairo/cairo.h>
#include <errno.h>
#include <glib.h>
#include <locale.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <syslog.h>
#include <unistd.h>
#ifdef XINERAMA
/* <X11/extensions/Xinerama.h> removed — xcb-xinerama used exclusively */
#include <xcb/xinerama.h>
#endif /* XINERAMA */
#ifdef XRANDR
/* <X11/extensions/Xrandr.h> removed — XCB randr used exclusively */
#endif /* XRANDR */
#ifdef COMPOSITOR
/* XComposite/XDamage/XFixes/XRender Xlib headers removed — compositor.c
 * uses pure XCB (xcb/composite.h, xcb/damage.h, xcb/xfixes.h, xcb/render.h) */
#endif /* COMPOSITOR */
#ifdef STATUSNOTIFIER
#include "sni.h"
#endif

#include "drw.h"
#include "icon.h"
#include "log.h"
#include "util.h"

/* macros */
#define BUTTONMASK \
	(XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_BUTTON_RELEASE)
#define CLEANMASK(mask)                                               \
	(mask & ~(g_plat.numlockmask | XCB_MOD_MASK_LOCK) &               \
	    (XCB_MOD_MASK_SHIFT | XCB_MOD_MASK_CONTROL | XCB_MOD_MASK_1 | \
	        XCB_MOD_MASK_2 | XCB_MOD_MASK_3 | XCB_MOD_MASK_4 |        \
	        XCB_MOD_MASK_5))
#define INTERSECT(x, y, w, h, m)                                     \
	(MAX(0, MIN((x) + (w), (m)->mx + (m)->mw) - MAX((x), (m)->mx)) * \
	    MAX(0, MIN((y) + (h), (m)->my + (m)->mh) - MAX((y), (m)->my)))
#define ISVISIBLE(C, M) ((C->tags & M->tagset[M->seltags]))
#define LENGTH(X) (sizeof X / sizeof X[0])
#define MOUSEMASK (BUTTONMASK | XCB_EVENT_MASK_POINTER_MOTION)
#define WIDTH(X) ((X)->w + 2 * (X)->bw)
#define HEIGHT(X) ((X)->h + 2 * (X)->bw)
#define TAGMASK ((1 << LENGTH(tags)) - 1)
#define TAGSLENGTH (LENGTH(tags))
#define TEXTW(X) (drw_fontset_getwidth(drw, (X)) + g_plat.lrpad)

/* Flush the X request buffer without a round-trip.
 * Prefer this over XSync(dpy, False) wherever error-draining is not needed. */
#define xflush() xcb_flush(g_plat.xc)

/* 1 when the compositor is compiled in, 0 otherwise. */
#ifdef COMPOSITOR
#define COMPOSITOR_ACTIVE 1
#else
#define COMPOSITOR_ACTIVE 0
#endif

/* Large enough for the fully assembled status2d string, including per-core
 * CPU vbar escapes (up to 64 cores × ~44 bytes each) plus all other widgets.
 * Must be >= STATUS_MAXLEN defined in status_config.h. */
#define STATUS_TEXT_LEN 8192

#define GAP_TOGGLE 100
#define GAP_RESET 0

/* XEMBED / systray defines */
#define SYSTEM_TRAY_REQUEST_DOCK 0
#define XEMBED_EMBEDDED_NOTIFY 0
#define XEMBED_MAPPED (1 << 0)
#define XEMBED_VERSION 0

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
	NetWMWindowTypeDock,
	NetWMWindowTypeToolbar,
	NetWMWindowTypeUtility,
	NetWMWindowTypeSplash,
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
	NetWMWindowOpacity,
	NetWMBypassCompositor,
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

#include "platform.h"
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

typedef struct Monitor Monitor;
typedef struct Client  Client;

/* Forward declaration — allows Client to hold a back-pointer to its CompWin
 * without requiring compositor_backend.h (internal to the compositor) to be
 * included here.  The full definition lives in compositor_backend.h. */
struct CompWin;

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
	int             ishidden;
	int             issteam;
	int             issni;
	char            scratchkey;
	double          opacity;           /* compositing opacity 0.0–1.0 */
	int             bypass_compositor; /* _NET_WM_BYPASS_COMPOSITOR hint */
	Client         *next;
	Client         *snext;
	Monitor        *mon;
	xcb_window_t    win;
	struct CompWin *cw; /* compositor back-pointer; NULL when untracked */
};

#ifdef COMPOSITOR
#include "compositor.h"
#endif /* COMPOSITOR */

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

/* Maximum per-tag slots — must be >= TAGSLENGTH at runtime.
 * Defined here (not in wmstate.h) so Monitor can use it as an array bound
 * before wmstate.h is included.  Must match WMSTATE_MAX_TAGS in wmstate.h. */
#define AWM_MAX_TAGS 16

/* Per-tag layout state embedded directly in Monitor — no heap allocation.
 * Previously a heap-allocated Pertag struct with 7 pointer fields; now a
 * plain value sub-struct so Monitor is trivially copyable. */
typedef struct {
	unsigned int  curtag, prevtag;
	int           nmasters[AWM_MAX_TAGS + 1];
	float         mfacts[AWM_MAX_TAGS + 1];
	unsigned int  sellts[AWM_MAX_TAGS + 1];
	const Layout *ltidxs[(AWM_MAX_TAGS + 1) * 2]; /* flattened [tag][0/1] */
	int           showbars[AWM_MAX_TAGS + 1];
	int           drawwithgaps[AWM_MAX_TAGS + 1];
	unsigned int  gappx[AWM_MAX_TAGS + 1];
} Pertag;

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
	Client       *sel;
	xcb_window_t  barwin;
	const Layout *lt[2];
	Pertag        pertag; /* inline — no heap allocation */
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
	double       opacity; /* 0.0 = use default (1.0); otherwise 0.0–1.0 */
} Rule;

typedef struct Systray Systray;
struct Systray {
	xcb_window_t   win;
	Client        *icons;
	xcb_visualid_t visual_id; /* 32-bit ARGB visual XID (0 = default) */
	xcb_colormap_t colormap;  /* colormap matching visual_id */
};

/* extern globals — defined in awm.c */
extern int      awm_tagslength; /* = TAGSLENGTH; set in setup() */
extern Drw     *drw;
extern Clr    **scheme;
extern Cur     *cursor[CurLast];
extern Systray *systray;
extern char     stext[STATUS_TEXT_LEN];
extern int      restart;
extern int      barsdirty;
extern int      launcher_visible; /* 1 while launcher window is open */
extern xcb_window_t
    launcher_xwin; /* X window ID of the launcher (from LAUNCHER_READY) */
extern xcb_timestamp_t
    last_event_time; /* timestamp of the most recent user event */
/* config-derived globals referenced by dbus.c / icon.c */
extern const unsigned int sniconsize;
extern const unsigned int iconcachesize;
extern const unsigned int iconcachemaxentries;
extern const unsigned int dbustimeout;
extern void (*handler[LASTEvent])(xcb_generic_event_t *);

/* Return the root_visual of screen number scr_num from an XCB connection.
 * Replaces XVisualIDFromVisual(DefaultVisual(dpy, screen)) everywhere. */
static inline xcb_visualid_t
xcb_screen_root_visual(xcb_connection_t *xc, int scr_num)
{
	xcb_screen_iterator_t it = xcb_setup_roots_iterator(xcb_get_setup(xc));
	for (int i = 0; i < scr_num; i++)
		xcb_screen_next(&it);
	return it.data->root_visual;
}

/* Return the root depth (bits-per-pixel) of screen number scr_num.
 * Replaces DefaultDepth(dpy, screen) everywhere. */
static inline uint8_t
xcb_screen_root_depth(xcb_connection_t *xc, int scr_num)
{
	xcb_screen_iterator_t it = xcb_setup_roots_iterator(xcb_get_setup(xc));
	for (int i = 0; i < scr_num; i++)
		xcb_screen_next(&it);
	return it.data->root_depth;
}

/* core WM functions (defined in awm.c) */
void quit(const Arg *arg);
void launchermenu(const Arg *arg);
void ui_send_monitor_geom(void);
void ui_send_theme(void);
void bar_hover_enter(Monitor *m);
void bar_hover_leave(void);
void preview_show_keybind(const Arg *arg);
void switcher_show(const Arg *arg);
void switcher_show_prev(const Arg *arg);

#endif /* AWM_H */
