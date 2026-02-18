/* AndrathWM - Application Launcher
 * See LICENSE file for copyright and license details.
 *
 * rofi-style launcher that reads .desktop files and falls back to PATH
 */

#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "drw.h"
#include "launcher.h"
#include "log.h"
#include "util.h"

#define LAUNCHER_INPUT_HEIGHT 28
#define LAUNCHER_ITEM_HEIGHT 24
#define LAUNCHER_PADDING 8
#define LAUNCHER_MIN_WIDTH 400
#define LAUNCHER_MAX_ITEMS 15
#define LAUNCHER_MAX_VISIBLE 12
#define LAUNCHER_SCROLL_BAR_WIDTH 6

static const char *desktop_paths[] = {
	"/usr/share/applications", "/usr/local/share/applications",
	NULL, /* will be replaced with ~/.local/share/applications */
	NULL, /* will be replaced with flatpak path */
};

static const char *skip_prefixes[] = {
	"gnome-",
	"kde-",
	"org.freedesktop.",
	"MIMEType",
	"Encoding",
};

static int
launcher_item_matches(LauncherItem *item, const char *input)
{
	if (!input || !*input)
		return 1;

	char name_lower[256];
	char input_lower[256];
	strncpy(name_lower, item->name, sizeof(name_lower) - 1);
	name_lower[sizeof(name_lower) - 1] = '\0';
	strncpy(input_lower, input, sizeof(input_lower) - 1);
	input_lower[sizeof(input_lower) - 1] = '\0';

	char *p = name_lower;
	while (*p) {
		*p = tolower(*p);
		p++;
	}
	p = input_lower;
	while (*p) {
		*p = tolower(*p);
		p++;
	}

	return strstr(name_lower, input_lower) != NULL;
}

static int
launcher_is_executable(const char *path)
{
	struct stat st;
	return stat(path, &st) == 0 && S_ISREG(st.st_mode) && (st.st_mode & 0111);
}

static int
launcher_is_desktop_entry(const char *filename)
{
	size_t len = strlen(filename);
	return len > 8 && strcmp(filename + len - 8, ".desktop") == 0;
}

static char *__attribute__((unused))
launcher_get_line(char *data, size_t len)
{
	char *newline = strchr(data, '\n');
	if (newline) {
		*newline = '\0';
		return newline + 1;
	}
	return NULL;
}

static char *
launcher_get_value(char *line, const char *key)
{
	size_t keylen = strlen(key);
	if (strncmp(line, key, keylen) == 0 && line[keylen] == '=') {
		return line + keylen + 1;
	}
	return NULL;
}

static int
launcher_should_skip_entry(const char *name)
{
	size_t i;
	for (i = 0; i < sizeof(skip_prefixes) / sizeof(skip_prefixes[0]); i++) {
		if (strncmp(name, skip_prefixes[i], strlen(skip_prefixes[i])) == 0)
			return 1;
	}
	return 0;
}

static LauncherItem *
launcher_parse_desktop_file(const char *path)
{
	FILE         *fp;
	char         *line = NULL;
	size_t        len  = 0;
	ssize_t       read;
	char         *name       = NULL;
	char         *exec_cmd   = NULL;
	char         *icon       = NULL;
	int           no_display = 0;
	LauncherItem *item       = NULL;

	fp = fopen(path, "r");
	if (!fp)
		return NULL;

	while ((read = getline(&line, &len, fp)) != -1) {
		if (line[0] == '[') {
			if (name && exec_cmd && strcmp(line, "[Desktop Entry]\n") != 0)
				break;
			continue;
		}

		if (!name) {
			char *v = launcher_get_value(line, "Name");
			if (v)
				name = strdup(v);
		}
		if (!exec_cmd) {
			char *v = launcher_get_value(line, "Exec");
			if (v)
				exec_cmd = strdup(v);
		}
		if (!icon) {
			char *v = launcher_get_value(line, "Icon");
			if (v)
				icon = strdup(v);
		}
		char *v = launcher_get_value(line, "NoDisplay");
		if (v && strcmp(v, "true") == 0)
			no_display = 1;
		v = launcher_get_value(line, "Hidden");
		if (v && strcmp(v, "true") == 0)
			no_display = 1;
	}

	free(line);
	fclose(fp);

	if (!name || !exec_cmd || no_display || launcher_should_skip_entry(name)) {
		if (name)
			free(name);
		if (exec_cmd)
			free(exec_cmd);
		if (icon)
			free(icon);
		return NULL;
	}

	item             = ecalloc(1, sizeof(LauncherItem));
	item->name       = name;
	item->exec       = exec_cmd;
	item->icon       = icon;
	item->is_desktop = 1;

	return item;
}

static LauncherItem *
launcher_load_desktop_files(const char *base_path)
{
	DIR           *dir;
	struct dirent *entry;
	LauncherItem  *items = NULL;
	LauncherItem  *last  = NULL;
	char           path[512];

	dir = opendir(base_path);
	if (!dir)
		return items;

	while ((entry = readdir(dir)) != NULL) {
		if (!launcher_is_desktop_entry(entry->d_name))
			continue;

		snprintf(path, sizeof(path), "%s/%s", base_path, entry->d_name);
		LauncherItem *item = launcher_parse_desktop_file(path);
		if (!item)
			continue;

		if (!items)
			items = item;
		else
			last->next = item;
		last = item;
	}

	closedir(dir);
	return items;
}

static LauncherItem *
launcher_find_duplicates(LauncherItem *items, const char *name)
{
	LauncherItem *item;
	for (item = items; item; item = item->next) {
		if (strcmp(item->name, name) == 0)
			return item;
	}
	return NULL;
}

static LauncherItem *__attribute__((unused))
launcher_merge_items(LauncherItem *a, LauncherItem *b)
{
	LauncherItem *result = NULL;
	LauncherItem *last   = NULL;

	while (a) {
		LauncherItem *next = a->next;
		a->next            = NULL;

		LauncherItem *dup = launcher_find_duplicates(b, a->name);
		if (dup) {
			if (dup->exec)
				free(dup->exec);
			if (dup->icon)
				free(dup->icon);
			dup->exec = a->exec;
			dup->icon = a->icon;
			free(a->name);
			free(a);
		} else {
			if (!result)
				result = a;
			else
				last->next = a;
			last = a;
		}
		a = next;
	}

	LauncherItem *tail = result;
	if (tail) {
		while (tail->next)
			tail = tail->next;
		tail->next = b;
	} else {
		result = b;
	}

	return result;
}

static LauncherItem *
launcher_scan_path(void)
{
	DIR           *dir;
	struct dirent *entry;
	LauncherItem  *items = NULL;
	LauncherItem  *last  = NULL;
	char          *path_env;
	char          *path_copy;
	char          *dir_path;
	char           full_path[1024];

	path_env = getenv("PATH");
	if (!path_env)
		return items;

	path_copy = strdup(path_env);
	if (!path_copy)
		return items;

	dir_path = strtok(path_copy, ":");
	while (dir_path) {
		dir = opendir(dir_path);
		if (dir) {
			while ((entry = readdir(dir)) != NULL) {
				if (entry->d_name[0] == '.')
					continue;

				snprintf(full_path, sizeof(full_path), "%s/%s", dir_path,
				    entry->d_name);

				if (!launcher_is_executable(full_path))
					continue;

				LauncherItem *dup =
				    launcher_find_duplicates(items, entry->d_name);
				if (dup)
					continue;

				LauncherItem *item = ecalloc(1, sizeof(LauncherItem));
				item->name         = strdup(entry->d_name);
				item->exec         = strdup(entry->d_name);
				item->is_desktop   = 0;

				if (!items)
					items = item;
				else
					last->next = item;
				last = item;
			}
			closedir(dir);
		}
		dir_path = strtok(NULL, ":");
	}

	free(path_copy);
	return items;
}

static void
launcher_filter_items(Launcher *launcher)
{
	LauncherItem *item;
	int           count = 0;

	if (launcher->filtered) {
		free(launcher->filtered);
		launcher->filtered = NULL;
	}

	for (item = launcher->items; item; item = item->next) {
		if (launcher_item_matches(item, launcher->input))
			count++;
	}

	if (count == 0) {
		launcher->visible_count = 0;
		launcher->selected      = -1;
		return;
	}

	launcher->filtered = ecalloc(count + 1, sizeof(LauncherItem *));
	count              = 0;
	for (item = launcher->items; item; item = item->next) {
		if (launcher_item_matches(item, launcher->input)) {
			launcher->filtered[count++] = item;
		}
	}
	launcher->filtered[count] = NULL;
	launcher->visible_count   = count;
	launcher->selected        = 0;
	launcher->scroll_offset   = 0;

	if (launcher->selected >= launcher->visible_count)
		launcher->selected = launcher->visible_count - 1;
}

static void
launcher_calculate_size(Launcher *launcher)
{
	LauncherItem *item;
	unsigned int  maxw = LAUNCHER_MIN_WIDTH;

	for (int i = 0; i < launcher->visible_count && launcher->filtered[i];
	     i++) {
		item           = launcher->filtered[i];
		unsigned int w = drw_fontset_getwidth(launcher->drw, item->name);
		if (w > maxw)
			maxw = w;
	}

	launcher->w = maxw + LAUNCHER_PADDING * 2 + LAUNCHER_SCROLL_BAR_WIDTH;
	launcher->h = LAUNCHER_INPUT_HEIGHT + LAUNCHER_PADDING * 2;
	if (launcher->visible_count > LAUNCHER_MAX_VISIBLE)
		launcher->h += LAUNCHER_MAX_VISIBLE * LAUNCHER_ITEM_HEIGHT;
	else if (launcher->visible_count > 0)
		launcher->h += launcher->visible_count * LAUNCHER_ITEM_HEIGHT;
	else
		launcher->h += LAUNCHER_ITEM_HEIGHT;
}

static void
launcher_render(Launcher *launcher)
{
	int x, y;

	if (!launcher->drw || !launcher->scheme || !launcher->visible)
		return;

	/* Resize window if needed */
	if (launcher->drw->w < launcher->w || launcher->drw->h < launcher->h)
		drw_resize(launcher->drw, launcher->w, launcher->h);

	drw_setscheme(launcher->drw, launcher->scheme[0]);
	drw_rect(launcher->drw, 0, 0, launcher->w, launcher->h, 1, 0);

	drw_setscheme(launcher->drw, launcher->scheme[1]);
	drw_rect(launcher->drw, 0, 0, launcher->w,
	    LAUNCHER_INPUT_HEIGHT + LAUNCHER_PADDING * 2, 1, 0);

	x = LAUNCHER_PADDING;
	y = LAUNCHER_PADDING;

	char input_display[260];
	int  len = launcher->input_len;
	if (len > (int) sizeof(input_display) - 1)
		len = sizeof(input_display) - 1;
	memcpy(input_display, launcher->input, len);
	input_display[len] = '\0';

	drw_text(launcher->drw, x, y, launcher->w - LAUNCHER_PADDING * 2,
	    LAUNCHER_INPUT_HEIGHT, 0, input_display, 0);

	y += LAUNCHER_INPUT_HEIGHT + LAUNCHER_PADDING;

	if (launcher->visible_count == 0) {
		drw_setscheme(launcher->drw, launcher->scheme[0]);
		drw_text(launcher->drw, x, y, launcher->w - LAUNCHER_PADDING * 2,
		    LAUNCHER_ITEM_HEIGHT, 0, "(no matches)", 0);
		drw_map(launcher->drw, launcher->win, 0, 0, launcher->w, launcher->h);
		XSync(launcher->dpy, False);
		return;
	}

	int start_idx = launcher->scroll_offset;
	int end_idx   = start_idx + LAUNCHER_MAX_VISIBLE;
	if (end_idx > launcher->visible_count)
		end_idx = launcher->visible_count;

	for (int i = start_idx; i < end_idx; i++) {
		LauncherItem *item        = launcher->filtered[i];
		int           is_selected = (i == launcher->selected);

		if (is_selected)
			drw_setscheme(launcher->drw, launcher->scheme[1]);
		else
			drw_setscheme(launcher->drw, launcher->scheme[0]);

		drw_rect(launcher->drw, 0, y, launcher->w, LAUNCHER_ITEM_HEIGHT, 1, 0);

		drw_text(launcher->drw, x, y, launcher->w - LAUNCHER_PADDING * 2,
		    LAUNCHER_ITEM_HEIGHT, 0, item->name, 0);

		y += LAUNCHER_ITEM_HEIGHT;
	}

	if (launcher->visible_count > LAUNCHER_MAX_VISIBLE) {
		unsigned int scroll_h = LAUNCHER_MAX_VISIBLE * LAUNCHER_ITEM_HEIGHT;
		unsigned int thumb_h =
		    (scroll_h * LAUNCHER_MAX_VISIBLE) / launcher->visible_count;
		unsigned int thumb_y =
		    (launcher->scroll_offset * scroll_h) / launcher->visible_count;

		drw_setscheme(launcher->drw, launcher->scheme[0]);
		drw_rect(launcher->drw, launcher->w - LAUNCHER_SCROLL_BAR_WIDTH - 2,
		    LAUNCHER_INPUT_HEIGHT + LAUNCHER_PADDING * 2, 2, scroll_h, 1, 0);

		drw_setscheme(launcher->drw, launcher->scheme[1]);
		drw_rect(launcher->drw, launcher->w - LAUNCHER_SCROLL_BAR_WIDTH - 2,
		    LAUNCHER_INPUT_HEIGHT + LAUNCHER_PADDING * 2 + thumb_y, 2, thumb_h,
		    1, 0);
	}

	drw_map(launcher->drw, launcher->win, 0, 0, launcher->w, launcher->h);
	XSync(launcher->dpy, False);
}

void
launcher_launch_selected(Launcher *launcher)
{
	if (!launcher || launcher->selected < 0 || !launcher->filtered)
		return;

	LauncherItem *item = launcher->filtered[launcher->selected];
	if (!item || !item->exec)
		return;

	launcher_hide(launcher);

	if (fork() == 0) {
		if (launcher->dpy)
			close(ConnectionNumber(launcher->dpy));
		setsid();
		execlp("sh", "sh", "-c", item->exec, NULL);
		exit(1);
	}
}

static void
launcher_append_items(Launcher *launcher, LauncherItem *new_items)
{
	LauncherItem *last;

	if (!new_items)
		return;

	launcher->item_count = 0;
	for (LauncherItem *i = launcher->items; i; i = i->next)
		launcher->item_count++;

	for (LauncherItem *i = new_items; i; i = i->next)
		launcher->item_count++;

	last = launcher->items;
	if (!last) {
		launcher->items = new_items;
	} else {
		while (last->next)
			last = last->next;
		last->next = new_items;
	}
}

Launcher *
launcher_create(Display *dpy, Window root, Drw *drw, Clr **scheme)
{
	Launcher            *launcher;
	XSetWindowAttributes wa;
	char                *home;
	char                 path[512];
	int                  i;

	launcher            = ecalloc(1, sizeof(Launcher));
	launcher->dpy       = dpy;
	launcher->drw       = drw;
	launcher->scheme    = scheme;
	launcher->selected  = -1;
	launcher->w         = LAUNCHER_MIN_WIDTH;
	launcher->h         = 100;
	launcher->input[0]  = '\0';
	launcher->input_len = 0;

	home = getenv("HOME");
	if (!home)
		home = "/root";

	for (i = 0; desktop_paths[i] != NULL || i < 2; i++) {
		if (desktop_paths[i] == NULL) {
			if (i == 2) {
				snprintf(
				    path, sizeof(path), "%s/.local/share/applications", home);
			} else if (i == 3) {
				snprintf(path, sizeof(path),
				    "%s/.local/share/flatpak/exports/share/applications",
				    home);
			} else {
				continue;
			}
		} else {
			snprintf(path, sizeof(path), "%s", desktop_paths[i]);
		}

		LauncherItem *items = launcher_load_desktop_files(path);
		launcher_append_items(launcher, items);
	}

	LauncherItem *path_items = launcher_scan_path();
	launcher_append_items(launcher, path_items);

	launcher_filter_items(launcher);
	launcher_calculate_size(launcher);

	wa.override_redirect = True;
	wa.background_pixel  = 0;
	wa.border_pixel      = 0;
	wa.colormap          = DefaultColormap(dpy, DefaultScreen(dpy));
	wa.event_mask        = ExposureMask | KeyPressMask | ButtonPressMask |
	    ButtonReleaseMask | PointerMotionMask;

	launcher->win = XCreateWindow(dpy, root, 0, 0, launcher->w, launcher->h, 1,
	    DefaultDepth(dpy, DefaultScreen(dpy)), CopyFromParent,
	    DefaultVisual(dpy, DefaultScreen(dpy)),
	    CWOverrideRedirect | CWBackPixel | CWBorderPixel | CWColormap |
	        CWEventMask,
	    &wa);

	return launcher;
}

void
launcher_free(Launcher *launcher)
{
	LauncherItem *item, *next;

	if (!launcher)
		return;

	if (launcher->visible)
		launcher_hide(launcher);

	if (launcher->win)
		XDestroyWindow(launcher->dpy, launcher->win);

	if (launcher->filtered)
		free(launcher->filtered);

	for (item = launcher->items; item; item = next) {
		next = item->next;
		free(item->name);
		free(item->exec);
		free(item->icon);
		free(item);
	}

	free(launcher);
}

void
launcher_show(Launcher *launcher, int x, int y)
{
	if (!launcher)
		return;

	launcher->x          = x;
	launcher->y          = y;
	launcher->input[0]   = '\0';
	launcher->input_len  = 0;
	launcher->cursor_pos = 0;

	launcher_filter_items(launcher);
	launcher_calculate_size(launcher);

	int scr   = DefaultScreen(launcher->dpy);
	int mon_x = 0, mon_y = 0, mon_w = DisplayWidth(launcher->dpy, scr),
	    mon_h = DisplayHeight(launcher->dpy, scr);

	if (launcher->x + launcher->w > mon_x + mon_w)
		launcher->x = mon_x + mon_w - launcher->w;
	if (launcher->y + launcher->h > mon_y + mon_h)
		launcher->y = mon_y + mon_h - launcher->h;
	if (launcher->x < mon_x)
		launcher->x = mon_x;
	if (launcher->y < mon_y)
		launcher->y = mon_y;

	XMoveResizeWindow(launcher->dpy, launcher->win, launcher->x, launcher->y,
	    launcher->w, launcher->h);
	XMapRaised(launcher->dpy, launcher->win);
	XSync(launcher->dpy, False);

	launcher->visible = 1;
	launcher_render(launcher);

	XGrabPointer(launcher->dpy, launcher->win, False,
	    ButtonPressMask | ButtonReleaseMask | PointerMotionMask, GrabModeAsync,
	    GrabModeAsync, None, None, CurrentTime);
	XGrabKeyboard(launcher->dpy, launcher->win, True, GrabModeAsync,
	    GrabModeAsync, CurrentTime);
}

void
launcher_hide(Launcher *launcher)
{
	if (!launcher || !launcher->visible)
		return;

	XUngrabPointer(launcher->dpy, CurrentTime);
	XUngrabKeyboard(launcher->dpy, CurrentTime);
	XUnmapWindow(launcher->dpy, launcher->win);
	launcher->visible = 0;
}

static void
launcher_insert_char(Launcher *launcher, char c)
{
	if (launcher->input_len >= (int) sizeof(launcher->input) - 1)
		return;

	if (launcher->cursor_pos < launcher->input_len) {
		memmove(launcher->input + launcher->cursor_pos + 1,
		    launcher->input + launcher->cursor_pos,
		    launcher->input_len - launcher->cursor_pos);
	}
	launcher->input[launcher->cursor_pos] = c;
	launcher->input_len++;
	launcher->cursor_pos++;
	launcher->input[launcher->input_len] = '\0';

	launcher_filter_items(launcher);
	launcher_calculate_size(launcher);
	XResizeWindow(launcher->dpy, launcher->win, launcher->w, launcher->h);
	launcher_render(launcher);
}

static void
launcher_delete_char(Launcher *launcher)
{
	if (launcher->cursor_pos <= 0 || launcher->input_len <= 0)
		return;

	launcher->input_len--;
	launcher->cursor_pos--;

	if (launcher->cursor_pos < launcher->input_len) {
		memmove(launcher->input + launcher->cursor_pos,
		    launcher->input + launcher->cursor_pos + 1,
		    launcher->input_len - launcher->cursor_pos);
	}
	launcher->input[launcher->input_len] = '\0';

	launcher_filter_items(launcher);
	launcher_calculate_size(launcher);
	XResizeWindow(launcher->dpy, launcher->win, launcher->w, launcher->h);
	launcher_render(launcher);
}

static void
launcher_move_cursor(Launcher *launcher, int delta)
{
	launcher->cursor_pos += delta;
	if (launcher->cursor_pos < 0)
		launcher->cursor_pos = 0;
	if (launcher->cursor_pos > launcher->input_len)
		launcher->cursor_pos = launcher->input_len;
}

static void
launcher_scroll(Launcher *launcher, int delta)
{
	int new_offset = launcher->scroll_offset + delta;

	if (new_offset < 0)
		new_offset = 0;
	if (new_offset + LAUNCHER_MAX_VISIBLE > launcher->visible_count)
		new_offset = launcher->visible_count - LAUNCHER_MAX_VISIBLE;
	if (new_offset < 0)
		new_offset = 0;

	launcher->scroll_offset = new_offset;

	if (launcher->selected < launcher->scroll_offset)
		launcher->selected = launcher->scroll_offset;
	if (launcher->selected >= launcher->scroll_offset + LAUNCHER_MAX_VISIBLE)
		launcher->selected =
		    launcher->scroll_offset + LAUNCHER_MAX_VISIBLE - 1;
}

int
launcher_handle_event(Launcher *launcher, XEvent *ev)
{
	KeySym key;
	int    idx, y;

	if (!launcher || !launcher->visible)
		return 0;

	if (ev->type == Expose) {
		if (ev->xexpose.count == 0)
			launcher_render(launcher);
		return 1;
	}

	if (ev->type == KeyPress) {
		key = XLookupKeysym(&ev->xkey, 0);

		switch (key) {
		case XK_Escape:
			launcher_hide(launcher);
			return 1;
		case XK_Return:
		case XK_KP_Enter:
			launcher_launch_selected(launcher);
			return 1;
		case XK_Up:
			if (launcher->selected > 0) {
				launcher->selected--;
				if (launcher->selected < launcher->scroll_offset)
					launcher_scroll(launcher, -1);
			}
			launcher_render(launcher);
			return 1;
		case XK_Down:
			if (launcher->selected < launcher->visible_count - 1) {
				launcher->selected++;
				if (launcher->selected >=
				    launcher->scroll_offset + LAUNCHER_MAX_VISIBLE)
					launcher_scroll(launcher, 1);
			}
			launcher_render(launcher);
			return 1;
		case XK_Page_Up:
			launcher_scroll(launcher, -LAUNCHER_MAX_VISIBLE);
			launcher_render(launcher);
			return 1;
		case XK_Page_Down:
			launcher_scroll(launcher, LAUNCHER_MAX_VISIBLE);
			launcher_render(launcher);
			return 1;
		case XK_Home:
			launcher->selected      = 0;
			launcher->scroll_offset = 0;
			launcher_render(launcher);
			return 1;
		case XK_End:
			launcher->selected = launcher->visible_count - 1;
			launcher->scroll_offset =
			    launcher->visible_count - LAUNCHER_MAX_VISIBLE;
			if (launcher->scroll_offset < 0)
				launcher->scroll_offset = 0;
			launcher_render(launcher);
			return 1;
		case XK_BackSpace:
			if (launcher->cursor_pos > 0) {
				launcher_delete_char(launcher);
			}
			return 1;
		case XK_Delete:
			if (launcher->cursor_pos < launcher->input_len) {
				launcher_delete_char(launcher);
			}
			return 1;
		case XK_Left:
			launcher_move_cursor(launcher, -1);
			launcher_render(launcher);
			return 1;
		case XK_Right:
			launcher_move_cursor(launcher, +1);
			launcher_render(launcher);
			return 1;
		case XK_Tab:
			if (ev->xkey.state & ShiftMask) {
				if (launcher->selected > 0) {
					launcher->selected--;
					if (launcher->selected < launcher->scroll_offset)
						launcher_scroll(launcher, -1);
				}
			} else {
				if (launcher->selected < launcher->visible_count - 1) {
					launcher->selected++;
					if (launcher->selected >=
					    launcher->scroll_offset + LAUNCHER_MAX_VISIBLE)
						launcher_scroll(launcher, 1);
				}
			}
			launcher_render(launcher);
			return 1;
		default:
			break;
		}

		if (ev->xkey.state & ControlMask) {
			switch (key) {
			case XK_u:
				launcher->input[0]   = '\0';
				launcher->input_len  = 0;
				launcher->cursor_pos = 0;
				launcher_filter_items(launcher);
				launcher_calculate_size(launcher);
				launcher_render(launcher);
				return 1;
			case XK_k:
				launcher->input[launcher->cursor_pos] = '\0';
				launcher->input_len                   = launcher->cursor_pos;
				launcher_filter_items(launcher);
				launcher_calculate_size(launcher);
				launcher_render(launcher);
				return 1;
			case XK_a:
				launcher->cursor_pos = 0;
				launcher_render(launcher);
				return 1;
			case XK_e:
				launcher->cursor_pos = launcher->input_len;
				launcher_render(launcher);
				return 1;
			}
		}

		/* Handle character input */
		{
			XComposeStatus status;
			char           buf[32];
			int            len;
			KeySym         ks;

			len = XLookupString(&ev->xkey, buf, sizeof(buf), &ks, &status);
			if (len > 0 && isprint((unsigned char) buf[0])) {
				launcher_insert_char(launcher, buf[0]);
			}
		}

		return 1;
	}

	if (ev->type == MotionNotify) {
		if (ev->xmotion.y <
		    (int) (LAUNCHER_INPUT_HEIGHT + LAUNCHER_PADDING * 2)) {
			launcher->selected = -1;
			launcher_render(launcher);
			return 1;
		}

		y   = LAUNCHER_INPUT_HEIGHT + LAUNCHER_PADDING * 2;
		idx = launcher->scroll_offset;

		while (idx < launcher->visible_count &&
		    y + LAUNCHER_ITEM_HEIGHT <= (int) launcher->h) {
			if (ev->xmotion.y >= y &&
			    ev->xmotion.y < y + LAUNCHER_ITEM_HEIGHT) {
				if (launcher->selected != idx) {
					launcher->selected = idx;
					launcher_render(launcher);
				}
				return 1;
			}
			y += LAUNCHER_ITEM_HEIGHT;
			idx++;
		}
		return 1;
	}

	if (ev->type == ButtonPress || ev->type == ButtonRelease) {
		if (ev->xbutton.y <
		    (int) (LAUNCHER_INPUT_HEIGHT + LAUNCHER_PADDING * 2)) {
			launcher_hide(launcher);
			return 1;
		}

		if (ev->type == ButtonRelease && launcher->selected >= 0) {
			launcher_launch_selected(launcher);
			return 1;
		}
		return 1;
	}

	return 0;
}
