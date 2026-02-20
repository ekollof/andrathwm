#include "movestack.c"
/* See LICENSE file for copyright and license details. */

/* appearance */
static const unsigned int borderpx        = 1; /* border pixel of windows */
static const int          startwithgaps[] = {
    1
}; /* 1 means gaps are used by default, this can be customized for each tag */
static const unsigned int gappx[] = {
	5
}; /* default gap between windows in pixels, this can be customized for each
      tag */
static const unsigned int snap = 32; /* snap pixel */
static const unsigned int systraypinning =
    0; /* 0: sloppy systray follows selected monitor, >0: pin systray to
          monitor X */
static const unsigned int systrayonleft =
    0; /* 0: systray in the right corner, >0: systray on left of status text */
static const unsigned int systrayspacing = 2; /* systray spacing */
static const int          systraypinningfailfirst =
    1; /* 1: if pinning fails, display systray on the first monitor, False:
          display systray on the last monitor*/
static const int showsystray = 1; /* 0 means no systray */
static const int showbar     = 1; /* 0 means no bar */
static const int topbar      = 1; /* 0 means bottom bar */

/* icon and performance settings */
static const unsigned int iconsize =
    16;                             /* size of client window icons in bar */
const unsigned int sniconsize = 22; /* size of StatusNotifier systray icons */
const unsigned int iconcachesize = 128; /* icon cache hash table size */
const unsigned int iconcachemaxentries =
    128; /* max cached icons before LRU eviction */
static const unsigned int motionfps =
    60; /* motion event throttle FPS (higher = more responsive) */
const unsigned int dbustimeout =
    100; /* D-Bus method call timeout in milliseconds */
static const char *fonts[] = {
	"BerkeleyMono Nerd Font 12",
};
static char  normbgcolor[]     = "#222222";
static char  normbordercolor[] = "#444444";
static char  normfgcolor[]     = "#bbbbbb";
static char  selfgcolor[]      = "#eeeeee";
static char  selbordercolor[]  = "#005577";
static char  selbgcolor[]      = "#005577";
static char  termcol0[]        = "#000000"; /* black   */
static char  termcol1[]        = "#ff0000"; /* red     */
static char  termcol2[]        = "#33ff00"; /* green   */
static char  termcol3[]        = "#ff0099"; /* yellow  */
static char  termcol4[]        = "#0066ff"; /* blue    */
static char  termcol5[]        = "#cc00ff"; /* magenta */
static char  termcol6[]        = "#00ffff"; /* cyan    */
static char  termcol7[]        = "#d0d0d0"; /* white   */
static char  termcol8[]        = "#808080"; /* black   */
static char  termcol9[]        = "#ff0000"; /* red     */
static char  termcol10[]       = "#33ff00"; /* green   */
static char  termcol11[]       = "#ff0099"; /* yellow  */
static char  termcol12[]       = "#0066ff"; /* blue    */
static char  termcol13[]       = "#cc00ff"; /* magenta */
static char  termcol14[]       = "#00ffff"; /* cyan    */
static char  termcol15[]       = "#ffffff"; /* white   */
static char *colors[][3]       = {
    /*               fg           bg           border   */
    [SchemeNorm] = { normfgcolor, normbgcolor, normbordercolor },
    [SchemeSel]  = { selfgcolor, selbgcolor, selbordercolor },
};

/* tagging */
static const char *tags[] = { "chat", "web", "shell", "work", "games", "dev",
	"mail", "misc", "doc" };

static const Rule rules[] = {
	/* xprop(1):
	 *	WM_CLASS(STRING) = instance, class
	 *	WM_NAME(STRING) = title
	 */
	/* class      instance    title       tags mask     iscentered   isfloating
	   monitor    scratch key */
	// { "Gimp",     NULL,       NULL,       0,            0,            1, -1,
	// 0  }, { "firefox",  NULL,       NULL,       1 << 8,       0, 0, -1, 0 },
	{ NULL, NULL, "notepad", 0, 1, 1, -1, 's' },
	{ NULL, NULL, "mpd", 0, 1, 1, -1, 'm' },
};

/* layout(s) */
static const float mfact = 0.50; /* factor of master area size [0.05..0.95] */
static const int   nmaster = 1;  /* number of clients in master area */
static const int   resizehints =
    1; /* 1 means respect size hints in tiled resizals */
static const int lockfullscreen =
    1; /* 1 will force focus on the fullscreen window */

static const Layout layouts[] = {
	/* symbol     arrange function */
	{ "TILE", tile }, /* first entry is default */
	{ "FULL", NULL }, /* no layout function means floating behavior */
	{ "MONO", monocle },
};

/* key definitions */
#define MODKEY XCB_MOD_MASK_4
#define ALTKEY XCB_MOD_MASK_1
#define TAGKEYS(KEY, TAG)                                                     \
	{ MODKEY, KEY, view, { .ui = 1 << TAG } },                                \
	    { MODKEY | XCB_MOD_MASK_CONTROL, KEY, toggleview,                     \
		    { .ui = 1 << TAG } },                                             \
	    { MODKEY | XCB_MOD_MASK_SHIFT, KEY, tag, { .ui = 1 << TAG } },        \
	    { MODKEY | XCB_MOD_MASK_CONTROL | XCB_MOD_MASK_SHIFT, KEY, toggletag, \
		    { .ui = 1 << TAG } },

/* helper for spawning shell commands in the pre dwm-5.0 fashion */
#define SHCMD(cmd)                     \
	{                                  \
		.v = (const char *[])          \
		{                              \
			"/bin/sh", "-c", cmd, NULL \
		}                              \
	}

/* commands */
static char dmenumon[2] =
    "0"; /* component of dmenucmd, manipulated in spawn() */
static const char *dmenucmd[] = { "rofi", "-show", "run", NULL };
static const char *termcmd[]  = { "st", NULL };

static const char *passmenu[]    = { "/usr/bin/env", "ksh", "-c",
	   "$HOME/bin/getpass", NULL };
static const char *lpassmenu[]   = { "/usr/bin/env", "ksh", "-c",
	  "$HOME/bin/lastpass-dmenu copy", NULL };
static const char *otpmenu[]     = { "/usr/bin/env", "ksh", "-c",
	    "$HOME/bin/getpass --totp", NULL };
static const char *screensaver[] = { "xscreensaver-command", "--lock", NULL };

static const char *wallpaper[] = { "/usr/bin/env", "ksh", "-c",
	"ksh $HOME/bin/wallpaper.ksh -r", NULL };

static const char *pickwall[] = { "/usr/bin/env", "ksh", "-c",
	"$HOME/bin/pickwall.sh", NULL };

static const char *layoutswitch[] = { "/usr/bin/env", "ksh", "-c",
	"$HOME/bin/setlayout", NULL };

static const char *windowswitch[] = { "/usr/bin/env", "ksh", "-c",
	"$HOME/bin/switch", NULL };

static const char *startbrowser[]   = { "chrome", NULL };
static const char *clipmenu[]       = { "clipmenu.sh", NULL };
static const char *networkmanager[] = { "networkmanager_dmenu", NULL };

/*
 * Scratch pads
 */
static const char  notepadname[] = "notepad";
static const char *notepadcmd[]  = { "s", "st", "-t", notepadname, "-g",
	 "120x34", "-e", "bash", "-c", "~/bin/scratchpad.sh", NULL };

static const char  musicname[] = "mpd";
static const char *musiccmd[]  = { "m", "st", "-t", musicname, "-g", "120x34",
	 "-e", "ksh", "-c", "ncmpcpp", NULL };

static const Key keys[] = {
	/* modifier                     key        function        argument */
	{ MODKEY, XKB_KEY_p, spawn, { .v = dmenucmd } },
	{ MODKEY, XKB_KEY_Return, spawn, { .v = termcmd } },
	{ MODKEY | XCB_MOD_MASK_SHIFT, XKB_KEY_p, spawn, { .v = passmenu } },
	{ MODKEY | XCB_MOD_MASK_SHIFT, XKB_KEY_l, spawn, { .v = lpassmenu } },
	{ MODKEY | XCB_MOD_MASK_SHIFT, XKB_KEY_o, spawn, { .v = otpmenu } },
	{ MODKEY | XCB_MOD_MASK_SHIFT, XKB_KEY_c, spawn, { .v = clipmenu } },
	{ MODKEY, XKB_KEY_w, spawn, { .v = startbrowser } },
	{ XCB_MOD_MASK_1 | XCB_MOD_MASK_CONTROL, XKB_KEY_l, spawn,
	    { .v = screensaver } },
	{ XCB_MOD_MASK_1 | XCB_MOD_MASK_CONTROL, XKB_KEY_w, spawn,
	    { .v = wallpaper } },
	{ MODKEY | XCB_MOD_MASK_CONTROL, XKB_KEY_w, spawn, { .v = pickwall } },
	{ MODKEY | XCB_MOD_MASK_SHIFT, XKB_KEY_l, spawn, { .v = layoutswitch } },
	{ MODKEY, XKB_KEY_n, spawn, { .v = networkmanager } },
	{ MODKEY | XCB_MOD_MASK_SHIFT, XKB_KEY_w, spawn, { .v = windowswitch } },
	{ MODKEY, XKB_KEY_grave, togglescratch, { .v = notepadcmd } },
	{ MODKEY, XKB_KEY_dead_grave, togglescratch, { .v = notepadcmd } },
	{ MODKEY | XCB_MOD_MASK_SHIFT, XKB_KEY_m, togglescratch,
	    { .v = musiccmd } },
	{ MODKEY, XKB_KEY_b, togglebar, { 0 } },
	{ MODKEY, XKB_KEY_j, focusstack, { .i = +1 } },
	{ MODKEY, XKB_KEY_k, focusstack, { .i = -1 } },
	{ MODKEY | XCB_MOD_MASK_SHIFT, XKB_KEY_j, focusstackhidden, { .i = +1 } },
	{ MODKEY | XCB_MOD_MASK_SHIFT, XKB_KEY_k, focusstackhidden, { .i = -1 } },
	{ MODKEY, XKB_KEY_i, incnmaster, { .i = +1 } },
	{ MODKEY, XKB_KEY_d, incnmaster, { .i = -1 } },
	{ MODKEY | XCB_MOD_MASK_CONTROL, XKB_KEY_h, setmfact, { .f = -0.05 } },
	{ MODKEY | XCB_MOD_MASK_CONTROL, XKB_KEY_l, setmfact, { .f = +0.05 } },
	{ MODKEY | XCB_MOD_MASK_CONTROL, XKB_KEY_j, movestack, { .i = +1 } },
	{ MODKEY | XCB_MOD_MASK_CONTROL, XKB_KEY_k, movestack, { .i = -1 } },
	{ MODKEY | XCB_MOD_MASK_SHIFT, XKB_KEY_Return, zoom, { 0 } },
	{ MODKEY, XKB_KEY_Tab, view, { 0 } },
	{ MODKEY, XKB_KEY_x, killclient, { 0 } },
	{ MODKEY, XKB_KEY_t, setlayout, { .v = &layouts[0] } },
	{ MODKEY, XKB_KEY_f, setlayout, { .v = &layouts[1] } },
	{ MODKEY, XKB_KEY_m, setlayout, { .v = &layouts[2] } },
	{ MODKEY, XKB_KEY_space, setlayout, { 0 } },
	{ MODKEY | XCB_MOD_MASK_SHIFT, XKB_KEY_space, togglefloating, { 0 } },
	{ MODKEY, XKB_KEY_0, view, { .ui = ~0 } },
	{ MODKEY | XCB_MOD_MASK_SHIFT, XKB_KEY_0, tag, { .ui = ~0 } },
	{ MODKEY, XKB_KEY_comma, focusmon, { .i = -1 } },
	{ MODKEY, XKB_KEY_period, focusmon, { .i = +1 } },
	{ MODKEY | XCB_MOD_MASK_SHIFT, XKB_KEY_comma, tagmon, { .i = -1 } },
	{ MODKEY | XCB_MOD_MASK_SHIFT, XKB_KEY_period, tagmon, { .i = +1 } },
	{ MODKEY, XKB_KEY_minus, setgaps, { .i = -5 } },
	{ MODKEY, XKB_KEY_equal, setgaps, { .i = +5 } },
	{ MODKEY | XCB_MOD_MASK_SHIFT, XKB_KEY_minus, setgaps,
	    { .i = GAP_RESET } },
	{ MODKEY | XCB_MOD_MASK_SHIFT, XKB_KEY_equal, setgaps,
	    { .i = GAP_TOGGLE } },
	{ MODKEY, XKB_KEY_F5, xrdb, { .v = NULL } },
	{ MODKEY, XKB_KEY_h, hidewin, { 0 } },
	{ MODKEY, XKB_KEY_s, restorewin, { 0 } },
	{ MODKEY | XCB_MOD_MASK_SHIFT, XKB_KEY_s, showall, { 0 } },
	TAGKEYS(XKB_KEY_1, 0) TAGKEYS(XKB_KEY_2, 1) TAGKEYS(XKB_KEY_3, 2)
	    TAGKEYS(XKB_KEY_4, 3) TAGKEYS(XKB_KEY_5, 4) TAGKEYS(XKB_KEY_6, 5)
	        TAGKEYS(XKB_KEY_7, 6) TAGKEYS(XKB_KEY_8, 7) TAGKEYS(XKB_KEY_9, 8) {
	            MODKEY | XCB_MOD_MASK_SHIFT, XKB_KEY_q, quit, { 0 } },
	{ MODKEY | XCB_MOD_MASK_SHIFT, XKB_KEY_r, quit, { 1 } },
};

/* button definitions */
/* click can be ClkTagBar, ClkLtSymbol, ClkStatusText, ClkWinTitle,
 * ClkClientWin, or ClkRootWin */
static const Button buttons[] = {
	/* click                event mask      button          function argument
	 */
	{ ClkLtSymbol, 0, XCB_BUTTON_INDEX_1, setlayout, { 0 } },
	{ ClkLtSymbol, 0, XCB_BUTTON_INDEX_3, setlayout, { .v = &layouts[2] } },
	{ ClkWinTitle, 0, XCB_BUTTON_INDEX_1, focuswin, { 0 } },
	{ ClkWinTitle, 0, XCB_BUTTON_INDEX_2, zoom, { 0 } },
	{ ClkStatusText, 0, XCB_BUTTON_INDEX_2, spawn, { .v = termcmd } },
	{ ClkClientWin, MODKEY, XCB_BUTTON_INDEX_1, movemouse, { 0 } },
	{ ClkClientWin, MODKEY, XCB_BUTTON_INDEX_2, togglefloating, { 0 } },
	{ ClkClientWin, MODKEY, XCB_BUTTON_INDEX_3, resizemouse, { 0 } },
	{ ClkTagBar, 0, XCB_BUTTON_INDEX_1, view, { 0 } },
	{ ClkTagBar, 0, XCB_BUTTON_INDEX_3, toggleview, { 0 } },
	{ ClkTagBar, MODKEY, XCB_BUTTON_INDEX_1, tag, { 0 } },
	{ ClkTagBar, MODKEY, XCB_BUTTON_INDEX_3, toggletag, { 0 } },
};

/* signal definitions */
/* signum must be greater than 0 */
/* trigger signals using `xsetroot -name "fsignal:<signum>"` */
static Signal signals[] = {
	/* signum       function        argument  */
	{ 1, setlayout, { .v = 0 } },
	{ 2, xrdb, { .v = 0 } },
};
