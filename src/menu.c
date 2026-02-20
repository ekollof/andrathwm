/* See LICENSE file for copyright and license details.
 *
 * Reusable menu system for awm
 */

#include <X11/Xlib.h>
#include <X11/Xlib-xcb.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#ifdef XINERAMA
#include <X11/extensions/Xinerama.h>
#endif
#include <xcb/xcb.h>
#include <xcb/xcb_keysyms.h>
#include <xcb/randr.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "drw.h"
#include "log.h"
#include "menu.h"
#include "awm.h"

#define MENU_ITEM_HEIGHT 22
#define MENU_PADDING 4
#define MENU_MIN_WIDTH 150
#define SEPARATOR_HEIGHT 8
#define MENU_TOGGLE_COL 16 /* width reserved for toggle indicator */

/* Internal helper to calculate menu dimensions */
static void
menu_calculate_size(Menu *menu)
{
	MenuItem    *item;
	unsigned int maxw       = MENU_MIN_WIDTH;
	unsigned int totalh     = MENU_PADDING * 2;
	int          count      = 0;
	int          has_toggle = 0;

	for (item = menu->items; item; item = item->next) {
		if (!item->is_separator && item->toggle_type != MENU_TOGGLE_NONE)
			has_toggle = 1;
	}

	for (item = menu->items; item; item = item->next) {
		if (item->is_separator) {
			totalh += SEPARATOR_HEIGHT;
		} else if (item->label) {
			unsigned int w = drw_fontset_getwidth(menu->drw, item->label);
			if (has_toggle)
				w += MENU_TOGGLE_COL;
			if (w > maxw)
				maxw = w;
			totalh += MENU_ITEM_HEIGHT;
			count++;
		}
	}

	menu->w          = maxw + MENU_PADDING * 4;
	menu->h          = totalh;
	menu->item_count = count;
}

/* Determine if submenu should open to the right (1) or left (0) */
static int
menu_submenu_opens_right(Menu *menu, MenuItem *item)
{
	unsigned int submenu_w;
	MenuItem    *subitem;

	if (!item || !item->submenu)
		return 1;

	/* Calculate submenu width */
	submenu_w = MENU_MIN_WIDTH;
	for (subitem = item->submenu; subitem; subitem = subitem->next) {
		if (subitem->label && *subitem->label) {
			unsigned int w = drw_fontset_getwidth(menu->drw, subitem->label);
			if (w > submenu_w)
				submenu_w = w;
		}
	}
	submenu_w += MENU_PADDING * 4;

	/* Check if there's space on the right */
	if (menu->x + menu->w + submenu_w <= menu->mon_x + menu->mon_w)
		return 1; /* Open to the right */

	/* Check if there's space on the left */
	if (menu->x - submenu_w >= menu->mon_x)
		return 0; /* Open to the left */

	/* Default to right if neither fits well */
	return 1;
}

/* Render the menu */
static void
menu_render(Menu *menu)
{
	MenuItem *item;
	int       y          = MENU_PADDING;
	int       idx        = 0;
	int       has_toggle = 0;

	if (!menu->drw || !menu->scheme)
		return;

	/* Check if any item has a toggle type (determines left-column layout) */
	for (item = menu->items; item; item = item->next) {
		if (!item->is_separator && item->toggle_type != MENU_TOGGLE_NONE) {
			has_toggle = 1;
			break;
		}
	}

	/* Ensure drawable is large enough for menu */
	if (menu->drw->w < menu->w || menu->drw->h < menu->h)
		drw_resize(menu->drw, menu->w, menu->h);

	/* Draw background */
	drw_setscheme(menu->drw, menu->scheme[0]); /* SchemeNorm */
	drw_rect(menu->drw, 0, 0, menu->w, menu->h, 1, 0);

	/* Draw items */
	for (item = menu->items; item; item = item->next) {
		if (item->is_separator) {
			/* Draw separator line */
			drw_setscheme(menu->drw, menu->scheme[0]);
			drw_rect(menu->drw, MENU_PADDING, y + SEPARATOR_HEIGHT / 2 - 1,
			    menu->w - MENU_PADDING * 2, 1, 1, 0);
			y += SEPARATOR_HEIGHT;
		} else if (item->label) {
			int is_selected = (idx == menu->selected);
			int text_x      = MENU_PADDING * 2;
			int text_w      = menu->w - MENU_PADDING * 4;

			/* Draw item background */
			if (is_selected && item->enabled) {
				drw_setscheme(menu->drw, menu->scheme[1]); /* SchemeSel */
			} else {
				drw_setscheme(menu->drw, menu->scheme[0]); /* SchemeNorm */
			}
			drw_rect(menu->drw, 0, y, menu->w, MENU_ITEM_HEIGHT, 1, 0);

			/* Draw toggle indicator in the left gutter */
			if (has_toggle) {
				const char *glyph = NULL;
				if (item->toggle_type == MENU_TOGGLE_CHECKMARK)
					glyph = item->toggle_state ? "✓" : " ";
				else if (item->toggle_type == MENU_TOGGLE_RADIO)
					glyph = item->toggle_state ? "●" : "○";
				if (glyph && glyph[0] != ' ')
					drw_text(menu->drw, MENU_PADDING, y, MENU_TOGGLE_COL,
					    MENU_ITEM_HEIGHT, 0, glyph, !item->enabled);
				text_x = MENU_PADDING + MENU_TOGGLE_COL;
				text_w = menu->w - text_x - MENU_PADDING * 2;
			}

			/* Draw item text */
			drw_text(menu->drw, text_x, y, text_w, MENU_ITEM_HEIGHT, 0,
			    item->label, !item->enabled);

			/* Draw submenu indicator if item has submenu */
			if (item->submenu) {
				const char *arrow =
				    menu_submenu_opens_right(menu, item) ? "►" : "◄";
				drw_text(menu->drw, menu->w - MENU_PADDING * 3, y,
				    MENU_PADDING * 2, MENU_ITEM_HEIGHT, 0, arrow,
				    !item->enabled);
			}

			/* Draw item text */
			drw_text(menu->drw, text_x, y, text_w, MENU_ITEM_HEIGHT, 0,
			    item->label, !item->enabled);

			/* Draw submenu indicator if item has submenu */
			if (item->submenu) {
				const char *arrow = menu_submenu_opens_right(menu, item)
				    ? "\xe2\x96\xba"
				    : "\xe2\x97\x84";
				drw_text(menu->drw, menu->w - MENU_PADDING * 3, y,
				    MENU_PADDING * 2, MENU_ITEM_HEIGHT, 0, arrow,
				    !item->enabled);
			}

			y += MENU_ITEM_HEIGHT;
			idx++;
		}
	}

	drw_map(menu->drw, menu->win, 0, 0, menu->w, menu->h);
}

/* Create menu */
Menu *
menu_create(Display *dpy, Window root, Drw *drw, Clr **scheme)
{
	Menu             *menu;
	xcb_connection_t *xc = XGetXCBConnection(dpy);
	uint32_t          mask;
	uint32_t          vals[5];
	xcb_colormap_t    cmap =
	    (xcb_colormap_t) DefaultColormap(dpy, DefaultScreen(dpy));
	xcb_visualid_t vid = (xcb_visualid_t) XVisualIDFromVisual(
	    DefaultVisual(dpy, DefaultScreen(dpy)));
	int depth = DefaultDepth(dpy, DefaultScreen(dpy));

	menu = calloc(1, sizeof(Menu));
	if (!menu)
		return NULL;

	menu->dpy                 = dpy;
	menu->drw                 = drw;
	menu->scheme              = scheme;
	menu->items               = NULL;
	menu->item_count          = 0;
	menu->owns_items          = 1; /* By default, menu owns its items */
	menu->selected            = -1;
	menu->visible             = 0;
	menu->ignore_next_release = 0;
	menu->parent              = NULL;
	menu->active_submenu      = NULL;
	menu->w                   = MENU_MIN_WIDTH;
	menu->h                   = 100;

	/* Create override-redirect window via XCB */
	menu->win = xcb_generate_id(xc);
	mask = XCB_CW_BACK_PIXEL | XCB_CW_BORDER_PIXEL | XCB_CW_OVERRIDE_REDIRECT |
	    XCB_CW_EVENT_MASK | XCB_CW_COLORMAP;
	vals[0] = 0; /* back pixel */
	vals[1] = 0; /* border pixel */
	vals[2] = 1; /* override redirect */
	vals[3] = XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_KEY_PRESS |
	    XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_BUTTON_RELEASE |
	    XCB_EVENT_MASK_POINTER_MOTION | XCB_EVENT_MASK_LEAVE_WINDOW |
	    XCB_EVENT_MASK_FOCUS_CHANGE;
	vals[4] = cmap;
	xcb_create_window(xc, (uint8_t) depth, menu->win, (xcb_window_t) root, 0,
	    0, (uint16_t) menu->w, (uint16_t) menu->h, 1,
	    XCB_WINDOW_CLASS_INPUT_OUTPUT, vid, mask, vals);

	return menu;
}

/* Free menu */
void
menu_free(Menu *menu)
{
	if (!menu)
		return;

	if (menu->visible)
		menu_hide(menu);

	if (menu->owns_items && menu->items)
		menu_items_free(menu->items);

	if (menu->win)
		xcb_destroy_window(XGetXCBConnection(menu->dpy), menu->win);

	free(menu);
}

/* Set menu items */
void
menu_set_items(Menu *menu, MenuItem *items)
{
	if (!menu)
		return;

	if (menu->owns_items && menu->items)
		menu_items_free(menu->items);

	menu->items    = items;
	menu->selected = -1;
	menu_calculate_size(menu);
}

/* Get monitor geometry containing point (x, y) */
static void
menu_get_monitor_geometry(
    Display *dpy, int x, int y, int *mon_x, int *mon_y, int *mon_w, int *mon_h)
{
	/* Default to full display */
	*mon_x = 0;
	*mon_y = 0;
	*mon_w = DisplayWidth(dpy, DefaultScreen(dpy));
	*mon_h = DisplayHeight(dpy, DefaultScreen(dpy));

#ifdef XRANDR
	{
		xcb_connection_t                  *xc = XGetXCBConnection(dpy);
		const xcb_query_extension_reply_t *ext =
		    xcb_get_extension_data(xc, &xcb_randr_id);

		if (ext && ext->present) {
			xcb_randr_get_screen_resources_cookie_t src;
			xcb_randr_get_screen_resources_reply_t *sr;
			xcb_randr_crtc_t                       *crtcs;
			int                                     ncrtc, i, found = 0;
			xcb_window_t root = (xcb_window_t) DefaultRootWindow(dpy);

			src = xcb_randr_get_screen_resources(xc, root);
			sr  = xcb_randr_get_screen_resources_reply(xc, src, NULL);
			if (sr) {
				ncrtc = xcb_randr_get_screen_resources_crtcs_length(sr);
				crtcs = xcb_randr_get_screen_resources_crtcs(sr);

				for (i = 0; i < ncrtc && !found; i++) {
					xcb_randr_get_crtc_info_cookie_t cic =
					    xcb_randr_get_crtc_info(
					        xc, crtcs[i], XCB_CURRENT_TIME);
					xcb_randr_get_crtc_info_reply_t *ci =
					    xcb_randr_get_crtc_info_reply(xc, cic, NULL);
					if (!ci)
						continue;
					if (ci->num_outputs > 0 && x >= ci->x &&
					    x < (int) (ci->x + ci->width) && y >= ci->y &&
					    y < (int) (ci->y + ci->height)) {
						*mon_x = ci->x;
						*mon_y = ci->y;
						*mon_w = ci->width;
						*mon_h = ci->height;
						found  = 1;
					}
					free(ci);
				}

				if (!found) {
					/* Point not in any CRTC — use first active one */
					for (i = 0; i < ncrtc && !found; i++) {
						xcb_randr_get_crtc_info_cookie_t cic =
						    xcb_randr_get_crtc_info(
						        xc, crtcs[i], XCB_CURRENT_TIME);
						xcb_randr_get_crtc_info_reply_t *ci =
						    xcb_randr_get_crtc_info_reply(xc, cic, NULL);
						if (!ci)
							continue;
						if (ci->num_outputs > 0) {
							*mon_x = ci->x;
							*mon_y = ci->y;
							*mon_w = ci->width;
							*mon_h = ci->height;
							found  = 1;
						}
						free(ci);
					}
				}
				free(sr);
				if (found)
					return;
			}
		}
	}
#endif /* XRANDR */

#ifdef XINERAMA
	{
		XineramaScreenInfo *screens;
		int                 nscreens, i;
		int                 found = 0;

		if (XineramaIsActive(dpy) &&
		    (screens = XineramaQueryScreens(dpy, &nscreens))) {
			for (i = 0; i < nscreens; i++) {
				if (x >= screens[i].x_org &&
				    x < screens[i].x_org + screens[i].width &&
				    y >= screens[i].y_org &&
				    y < screens[i].y_org + screens[i].height) {
					*mon_x = screens[i].x_org;
					*mon_y = screens[i].y_org;
					*mon_w = screens[i].width;
					*mon_h = screens[i].height;
					found  = 1;
					break;
				}
			}
			if (!found && nscreens > 0) {
				*mon_x = screens[0].x_org;
				*mon_y = screens[0].y_org;
				*mon_w = screens[0].width;
				*mon_h = screens[0].height;
			}
			XFree(screens);
		}
	}
#else
	(void) x;
	(void) y;
#endif /* XINERAMA */
}

/* Show menu at coordinates */
void
menu_show(Menu *menu, int x, int y, MenuCallback callback, void *data,
    Time event_time)
{
	int mon_x, mon_y, mon_w, mon_h;

	if (!menu || !menu->items)
		return;

	menu->callback      = callback;
	menu->callback_data = data;

	/* Recalculate size in case items changed */
	menu_calculate_size(menu);

	/* Get monitor geometry containing the click point */
	menu_get_monitor_geometry(menu->dpy, x, y, &mon_x, &mon_y, &mon_w, &mon_h);

	/* Store monitor bounds for submenu positioning */
	menu->mon_x = mon_x;
	menu->mon_y = mon_y;
	menu->mon_w = mon_w;
	menu->mon_h = mon_h;

	menu->x = x;
	menu->y = y;

	awm_debug("Menu: Initial pos (%d,%d) size %ux%u, monitor [%d,%d %dx%d]", x,
	    y, menu->w, menu->h, mon_x, mon_y, mon_w, mon_h);

	/* Ensure menu fits within monitor bounds */
	if (menu->x + menu->w > mon_x + mon_w)
		menu->x = mon_x + mon_w - menu->w;
	if (menu->y + menu->h > mon_y + mon_h)
		menu->y = mon_y + mon_h - menu->h;
	if (menu->x < mon_x)
		menu->x = mon_x;
	if (menu->y < mon_y)
		menu->y = mon_y;

	awm_debug("Menu: Adjusted pos (%d,%d)", menu->x, menu->y);

	/* Position and show window */
	{
		xcb_connection_t *xc = XGetXCBConnection(menu->dpy);
		uint32_t          cfg[4];

		cfg[0] = (uint32_t) menu->x;
		cfg[1] = (uint32_t) menu->y;
		cfg[2] = menu->w;
		cfg[3] = menu->h;
		xcb_configure_window(xc, menu->win,
		    XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
		        XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT,
		    cfg);
		xcb_map_window(xc, menu->win);
		cfg[0] = XCB_STACK_MODE_ABOVE;
		xcb_configure_window(xc, menu->win, XCB_CONFIG_WINDOW_STACK_MODE, cfg);
	}

	/* Process expose to render before grab */
	xcb_flush(XGetXCBConnection(menu->dpy));

	menu->visible             = 1;
	menu->ignore_next_release = 1; /* Ignore the pending ButtonRelease */
	menu_render(menu);

	/* Ungrab any existing pointer grab (e.g. from Electron) using the
	 * original event timestamp, then immediately re-grab.  Qt and GTK
	 * use exactly this approach: issuing a new XCB_GRAB_MODE_ASYNC grab
	 * with the triggering event's timestamp steals the grab from whoever
	 * held it (the async/async combination is key — it replaces another
	 * client's async grab).  Using CurrentTime here would fail with
	 * AlreadyGrabbed because X11 rejects a grab that pre-dates the
	 * existing one. */
	{
		xcb_connection_t          *xc = XGetXCBConnection(menu->dpy);
		xcb_grab_pointer_cookie_t  gpc;
		xcb_grab_pointer_reply_t  *gpr;
		xcb_grab_keyboard_cookie_t gkc;
		xcb_grab_keyboard_reply_t *gkr;

		xcb_ungrab_pointer(xc, (xcb_timestamp_t) event_time);
		xcb_flush(xc);

		gpc = xcb_grab_pointer(xc, 0, menu->win,
		    XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_BUTTON_RELEASE |
		        XCB_EVENT_MASK_POINTER_MOTION,
		    XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC, XCB_NONE, XCB_NONE,
		    (xcb_timestamp_t) event_time);
		gpr = xcb_grab_pointer_reply(xc, gpc, NULL);
		if (!gpr || gpr->status != XCB_GRAB_STATUS_SUCCESS)
			awm_warn("Menu: Failed to grab pointer (status=%d)",
			    gpr ? gpr->status : -1);
		free(gpr);

		gkc = xcb_grab_keyboard(xc, 1, menu->win, (xcb_timestamp_t) event_time,
		    XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC);
		gkr = xcb_grab_keyboard_reply(xc, gkc, NULL);
		if (!gkr || gkr->status != XCB_GRAB_STATUS_SUCCESS)
			awm_warn("Menu: Failed to grab keyboard (status=%d)",
			    gkr ? gkr->status : -1);
		free(gkr);
	}
}

/* Show submenu for a menu item */
static void
menu_show_submenu(Menu *parent, MenuItem *item, int item_y)
{
	Menu *submenu;
	int   sub_x, sub_y;

	if (!parent || !item || !item->submenu)
		return;

	/* Close any existing submenu */
	if (parent->active_submenu) {
		menu_hide(parent->active_submenu);
		menu_free(parent->active_submenu);
		parent->active_submenu = NULL;
	}

	/* Create new submenu */
	submenu = menu_create(parent->dpy, DefaultRootWindow(parent->dpy),
	    parent->drw, parent->scheme);
	if (!submenu)
		return;

	/* Set up submenu */
	submenu->parent     = parent;
	submenu->owns_items = 0; /* Submenu items are owned by parent MenuItem */
	submenu->mon_x      = parent->mon_x;
	submenu->mon_y      = parent->mon_y;
	submenu->mon_w      = parent->mon_w;
	submenu->mon_h      = parent->mon_h;
	menu_set_items(submenu, item->submenu);
	menu_calculate_size(submenu);

	/* Position submenu */
	if (menu_submenu_opens_right(parent, item)) {
		sub_x = parent->x + parent->w;
	} else {
		sub_x = parent->x - submenu->w;
	}
	sub_y = parent->y + item_y;

	/* Adjust for monitor bounds */
	if (sub_x + submenu->w > parent->mon_x + parent->mon_w)
		sub_x = parent->mon_x + parent->mon_w - submenu->w;
	if (sub_x < parent->mon_x)
		sub_x = parent->mon_x;
	if (sub_y + submenu->h > parent->mon_y + parent->mon_h)
		sub_y = parent->mon_y + parent->mon_h - submenu->h;
	if (sub_y < parent->mon_y)
		sub_y = parent->mon_y;

	/* Show submenu without callback (parent handles activation) */
	submenu->x             = sub_x;
	submenu->y             = sub_y;
	submenu->callback      = parent->callback;
	submenu->callback_data = parent->callback_data;

	{
		xcb_connection_t *xc = XGetXCBConnection(submenu->dpy);
		uint32_t          cfg[4];

		cfg[0] = (uint32_t) submenu->x;
		cfg[1] = (uint32_t) submenu->y;
		cfg[2] = submenu->w;
		cfg[3] = submenu->h;
		xcb_configure_window(xc, submenu->win,
		    XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
		        XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT,
		    cfg);
		xcb_map_window(xc, submenu->win);
		cfg[0] = XCB_STACK_MODE_ABOVE;
		xcb_configure_window(
		    xc, submenu->win, XCB_CONFIG_WINDOW_STACK_MODE, cfg);
	}
	menu_render(submenu);

	parent->active_submenu = submenu;
}

/* Hide menu */
void
menu_hide(Menu *menu)
{
	if (!menu || !menu->visible)
		return;

	/* Hide any active submenu first */
	if (menu->active_submenu) {
		menu_hide(menu->active_submenu);
		menu_free(menu->active_submenu);
		menu->active_submenu = NULL;
	}

	{
		xcb_connection_t *xc = XGetXCBConnection(menu->dpy);
		xcb_ungrab_pointer(xc, XCB_CURRENT_TIME);
		xcb_ungrab_keyboard(xc, XCB_CURRENT_TIME);
		xcb_unmap_window(xc, menu->win);
	}
	menu->visible  = 0;
	menu->selected = -1;
}

/* Handle X event - returns 1 if event was handled */
int
menu_handle_event(Menu *menu, XEvent *ev)
{
	MenuItem *item;
	int       idx, y;

	if (!menu || !menu->visible)
		return 0;

	/* Check submenu first */
	if (menu->active_submenu && menu_handle_event(menu->active_submenu, ev))
		return 1;

	/* Check if event is for our window */
	if (ev->xany.window != menu->win) {
		/* Ignore ALL button events until after the initial release */
		if (menu->ignore_next_release) {
			if (ev->type == ButtonPress || ev->type == ButtonRelease) {
				if (ev->type == ButtonRelease) {
					menu->ignore_next_release = 0;
				}
				return 1;
			}
		} else {
			/* After initial release, any button event outside menu means close
			 * it */
			if (ev->type == ButtonPress || ev->type == ButtonRelease) {
				menu_hide(menu);
				return 1;
			}
		}
		return 0;
	}

	switch (ev->type) {
	case Expose:
		if (ev->xexpose.count == 0)
			menu_render(menu);
		return 1;

	case MotionNotify:
		/* Update selection based on mouse position */
		/* Only process if mouse is within menu bounds */
		if (ev->xmotion.x < 0 || ev->xmotion.x >= (int) menu->w ||
		    ev->xmotion.y < 0 || ev->xmotion.y >= (int) menu->h) {
			/* Mouse outside menu - clear selection */
			if (menu->selected != -1) {
				menu->selected = -1;
				menu_render(menu);
			}
			return 1;
		}

		y   = MENU_PADDING;
		idx = 0;
		for (item = menu->items; item; item = item->next) {
			if (item->is_separator) {
				y += SEPARATOR_HEIGHT;
			} else if (item->label) {
				if (ev->xmotion.y >= y &&
				    ev->xmotion.y < y + MENU_ITEM_HEIGHT) {
					if (menu->selected != idx) {
						menu->selected = idx;
						menu_render(menu);

						/* Show submenu if this item has one */
						if (item->submenu) {
							menu_show_submenu(menu, item, y);
						} else if (menu->active_submenu) {
							/* Close submenu if moving to item without submenu
							 */
							menu_hide(menu->active_submenu);
							menu_free(menu->active_submenu);
							menu->active_submenu = NULL;
						}
					}
					return 1;
				}
				y += MENU_ITEM_HEIGHT;
				idx++;
			}
		}
		/* Mouse not over any item */
		if (menu->selected != -1) {
			menu->selected = -1;
			/* Close any open submenu */
			if (menu->active_submenu) {
				menu_hide(menu->active_submenu);
				menu_free(menu->active_submenu);
				menu->active_submenu = NULL;
			}
			menu_render(menu);
		}
		return 1;

	case ButtonRelease:
		/* Ignore first release after showing menu */
		if (menu->ignore_next_release) {
			menu->ignore_next_release = 0;
			return 1;
		}
		return 1;

	case ButtonPress:
		/* Check if click is outside menu bounds using root coordinates */
		if (ev->xbutton.x_root < menu->x ||
		    ev->xbutton.x_root >= menu->x + (int) menu->w ||
		    ev->xbutton.y_root < menu->y ||
		    ev->xbutton.y_root >= menu->y + (int) menu->h) {
			menu_hide(menu);
			return 1;
		}
		/* Click on menu item */
		if (menu->selected >= 0) {
			/* Find the selected item */
			idx = 0;
			for (item = menu->items; item; item = item->next) {
				if (!item->is_separator && item->label) {
					if (idx == menu->selected && item->enabled) {
						/* Don't activate items with submenus - they only open
						 * the submenu */
						if (item->submenu) {
							return 1;
						}
						/* Found it - trigger callback */
						if (menu->callback)
							menu->callback(item->id, menu->callback_data);
						menu_hide(menu);
						return 1;
					}
					idx++;
				}
			}
		}
		return 1;

	case KeyPress: {
		xcb_keysym_t key = xcb_key_symbols_get_keysym(
		    keysyms, (xcb_keycode_t) ev->xkey.keycode, 0);
		switch (key) {
		case XK_Escape:
			menu_hide(menu);
			return 1;
		case XK_Up:
			/* Move selection up */
			if (menu->selected > 0)
				menu->selected--;
			else
				menu->selected = menu->item_count - 1;
			menu_render(menu);
			return 1;
		case XK_Down:
			/* Move selection down */
			if (menu->selected < menu->item_count - 1)
				menu->selected++;
			else
				menu->selected = 0;
			menu_render(menu);
			return 1;
		case XK_Return:
		case XK_KP_Enter:
			/* Activate selected item */
			if (menu->selected >= 0) {
				idx = 0;
				for (item = menu->items; item; item = item->next) {
					if (!item->is_separator && item->label) {
						if (idx == menu->selected && item->enabled) {
							if (menu->callback)
								menu->callback(item->id, menu->callback_data);
							menu_hide(menu);
							return 1;
						}
						idx++;
					}
				}
			}
			return 1;
		}
	}
		return 1;

	case LeaveNotify:
		/* Clear selection when mouse leaves */
		if (menu->selected != -1) {
			menu->selected = -1;
			menu_render(menu);
		}
		return 1;

	case FocusOut:
		/*
		 * Dismiss menu when keyboard focus leaves — this is the fallback
		 * dismiss path when XGrabPointer/XGrabKeyboard failed (e.g. because
		 * an Electron window held the grab).  Ignore grab-related transient
		 * focus events (NotifyGrab / NotifyUngrab / NotifyWhileGrabbed) and
		 * focus moving to an inferior window (submenu).
		 */
		if (ev->xfocus.detail != NotifyInferior &&
		    ev->xfocus.mode != NotifyGrab && ev->xfocus.mode != NotifyUngrab) {
			menu_hide(menu);
		}
		return 1;
	}

	return 0;
}

/* Menu item helpers */
MenuItem *
menu_item_create(int id, const char *label, int enabled)
{
	MenuItem *item = calloc(1, sizeof(MenuItem));
	if (!item)
		return NULL;

	item->id           = id;
	item->label        = label ? strdup(label) : NULL;
	item->enabled      = enabled;
	item->is_separator = 0;
	item->submenu      = NULL;
	item->next         = NULL;

	return item;
}

MenuItem *
menu_separator_create(void)
{
	MenuItem *item = calloc(1, sizeof(MenuItem));
	if (!item)
		return NULL;

	item->is_separator = 1;
	item->enabled      = 0;
	item->next         = NULL;

	return item;
}

void
menu_item_free(MenuItem *item)
{
	if (!item)
		return;

	if (item->label)
		free(item->label);
	if (item->submenu)
		menu_items_free(item->submenu);
	free(item);
}

void
menu_items_free(MenuItem *head)
{
	MenuItem *item, *next;

	for (item = head; item; item = next) {
		next = item->next;
		menu_item_free(item);
	}
}

int
menu_items_count(MenuItem *head)
{
	MenuItem *item;
	int       count = 0;

	for (item = head; item; item = item->next) {
		if (!item->is_separator && item->label)
			count++;
	}

	return count;
}
