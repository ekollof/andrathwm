/* AndrathWM - Application Launcher
 * See LICENSE file for copyright and license details.
 *
 * rofi-style launcher that reads .desktop files and falls back to PATH.
 * GTK backend: GtkWindow (undecorated, POPUP_MENU) + GtkSearchEntry +
 * GtkScrolledWindow + GtkListBox.
 *
 * Everything related to item discovery, icon loading, history, and launching
 * is identical to the original implementation.  Only the UI layer has
 * changed: XCB windows / drw_* calls are replaced with GTK widgets driven
 * by the GLib main loop that already runs in awm.c.
 */

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <gtk/gtk.h>
#include <xcb/xcb.h>

#include "icon.h"
#include "launcher.h"
#include "log.h"
#include "util.h"

/* -------------------------------------------------------------------------
 * Desktop file scanning (unchanged from original)
 * ---------------------------------------------------------------------- */

static const char *desktop_paths[] = {
	"/usr/share/applications",
	"/usr/local/share/applications",
	NULL, /* replaced at runtime with ~/.local/share/applications */
	NULL, /* replaced at runtime with flatpak path */
};

static const char *skip_prefixes[] = {
	"gnome-",
	"kde-",
	"org.freedesktop.",
	"MIMEType",
	"Encoding",
};

/* -------------------------------------------------------------------------
 * Icon alias table (unchanged)
 * ---------------------------------------------------------------------- */

typedef struct IconAlias {
	char             *short_lower;
	char             *full_name;
	struct IconAlias *next;
} IconAlias;

#define ICON_ALIAS_BUCKETS 256
static IconAlias *icon_alias_table[ICON_ALIAS_BUCKETS];
static int        icon_alias_built = 0;

static unsigned int
icon_alias_hash(const char *s)
{
	unsigned int h = 5381;
	int          c;
	while ((c = *s++))
		h = ((h << 5) + h) + (unsigned char) tolower(c);
	return h % ICON_ALIAS_BUCKETS;
}

static void
icon_alias_build(void)
{
	GtkIconTheme *theme;
	GList        *all, *l;

	if (icon_alias_built)
		return;
	icon_alias_built = 1;

	theme = gtk_icon_theme_get_default();
	if (!theme)
		return;

	all = gtk_icon_theme_list_icons(theme, NULL);
	for (l = all; l; l = l->next) {
		const char *name = (const char *) l->data;
		const char *dot  = strrchr(name, '.');
		if (!dot)
			continue;
		const char *last = dot + 1;
		char        lower[128];
		size_t      i;
		for (i = 0; last[i] && i < sizeof(lower) - 1; i++)
			lower[i] = (char) tolower((unsigned char) last[i]);
		lower[i] = '\0';

		unsigned int bucket = icon_alias_hash(lower);
		IconAlias   *a      = malloc(sizeof(IconAlias));
		if (!a)
			continue;
		a->short_lower           = strdup(lower);
		a->full_name             = strdup(name);
		a->next                  = icon_alias_table[bucket];
		icon_alias_table[bucket] = a;
	}
	g_list_free_full(all, g_free);
}

static void
icon_alias_free(void)
{
	int        i;
	IconAlias *a, *next;

	for (i = 0; i < ICON_ALIAS_BUCKETS; i++) {
		for (a = icon_alias_table[i]; a; a = next) {
			next = a->next;
			free(a->short_lower);
			free(a->full_name);
			free(a);
		}
		icon_alias_table[i] = NULL;
	}
	icon_alias_built = 0;
}

static const char *
icon_alias_lookup(const char *short_name)
{
	char         lower[128];
	size_t       i;
	unsigned int bucket;
	IconAlias   *a;

	for (i = 0; short_name[i] && i < sizeof(lower) - 1; i++)
		lower[i] = (char) tolower((unsigned char) short_name[i]);
	lower[i] = '\0';

	bucket = icon_alias_hash(lower);
	for (a = icon_alias_table[bucket]; a; a = a->next) {
		if (strcmp(a->short_lower, lower) == 0)
			return a->full_name;
	}
	return NULL;
}

static cairo_surface_t *
launcher_load_icon(const char *icon_name, int size)
{
	cairo_surface_t *surface;
	const char      *resolved;

	if (!icon_name || !*icon_name)
		return NULL;

	surface = icon_load(icon_name, size);
	if (surface)
		return surface;

	icon_alias_build();
	resolved = icon_alias_lookup(icon_name);
	if (!resolved)
		return NULL;

	return icon_load(resolved, size);
}

/* -------------------------------------------------------------------------
 * Item matching / sorting (unchanged)
 * ---------------------------------------------------------------------- */

static int
launcher_item_matches(LauncherItem *item, const char *input)
{
	char name_lower[256];
	char input_lower[256];

	if (!input || !*input)
		return 1;

	strncpy(name_lower, item->name, sizeof(name_lower) - 1);
	name_lower[sizeof(name_lower) - 1] = '\0';
	strncpy(input_lower, input, sizeof(input_lower) - 1);
	input_lower[sizeof(input_lower) - 1] = '\0';

	char *p = name_lower;
	while (*p) {
		*p = (char) tolower((unsigned char) *p);
		p++;
	}
	p = input_lower;
	while (*p) {
		*p = (char) tolower((unsigned char) *p);
		p++;
	}

	return strstr(name_lower, input_lower) != NULL;
}

static int
launcher_item_cmp(const void *a, const void *b)
{
	const LauncherItem *ia = *(const LauncherItem *const *) a;
	const LauncherItem *ib = *(const LauncherItem *const *) b;
	int                 ca = ia->launch_count > 0 ? ia->launch_count : 0;
	int                 cb = ib->launch_count > 0 ? ib->launch_count : 0;

	if (ca > 0 && cb > 0)
		return (cb > ca) - (ca > cb);
	if (ca > 0)
		return -1;
	if (cb > 0)
		return 1;

	return strcasecmp(ia->name, ib->name);
}

/* -------------------------------------------------------------------------
 * Desktop file parsing helpers (unchanged)
 * ---------------------------------------------------------------------- */

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

static void
exec_strip_field_codes(char *s)
{
	char *r = s, *w = s;

	while (*r) {
		if (*r == '%' && *(r + 1) != '\0') {
			char code = *(r + 1);
			if (code == '%') {
				*w++ = '%';
				r += 2;
			} else if (code == 'u' || code == 'U' || code == 'f' ||
			    code == 'F' || code == 'd' || code == 'D' || code == 'n' ||
			    code == 'N' || code == 'v' || code == 'm' || code == 'k' ||
			    code == 'c' || code == 'i') {
				r += 2;
				if (*r == ' ')
					r++;
			} else {
				*w++ = *r++;
			}
		} else {
			*w++ = *r++;
		}
	}
	while (w > s && *(w - 1) == ' ')
		w--;
	*w = '\0';
}

static char *
launcher_get_value(char *line, const char *key)
{
	size_t keylen = strlen(key);
	if (strncmp(line, key, keylen) == 0 && line[keylen] == '=') {
		char  *val  = line + keylen + 1;
		size_t vlen = strlen(val);
		while (vlen > 0 && (val[vlen - 1] == '\n' || val[vlen - 1] == '\r'))
			val[--vlen] = '\0';
		return val;
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
	const char *basename = strrchr(path, '/');
	basename             = basename ? basename + 1 : path;

	if (launcher_should_skip_entry(basename))
		return NULL;

	FILE         *fp;
	char         *line = NULL;
	size_t        len  = 0;
	ssize_t       nread;
	char         *name       = NULL;
	char         *exec_cmd   = NULL;
	char         *icon       = NULL;
	int           no_display = 0;
	int           terminal   = 0;
	LauncherItem *item       = NULL;

	fp = fopen(path, "r");
	if (!fp)
		return NULL;

	while ((nread = getline(&line, &len, fp)) != -1) {
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
			if (v) {
				exec_cmd = strdup(v);
				if (exec_cmd)
					exec_strip_field_codes(exec_cmd);
			}
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
		v = launcher_get_value(line, "Terminal");
		if (v && strcmp(v, "true") == 0)
			terminal = 1;
	}

	free(line);
	fclose(fp);

	if (!name || !exec_cmd || no_display) {
		free(name);
		free(exec_cmd);
		free(icon);
		return NULL;
	}

	item            = ecalloc(1, sizeof(LauncherItem));
	item->name      = name;
	item->exec      = exec_cmd;
	item->icon_name = icon;
	item->icon = icon ? launcher_load_icon(icon, LAUNCHER_ICON_SIZE) : NULL;
	if (icon && !item->icon)
		awm_debug("Launcher: failed to load icon '%s'", icon);
	item->is_desktop = 1;
	item->terminal   = terminal;

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

static LauncherItem *
launcher_scan_path(LauncherItem *existing)
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

				if (launcher_find_duplicates(existing, entry->d_name))
					continue;
				if (launcher_find_duplicates(items, entry->d_name))
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

/* -------------------------------------------------------------------------
 * History (unchanged)
 * ---------------------------------------------------------------------- */

static void
launcher_history_path(char *out, size_t sz)
{
	const char *state = getenv("XDG_STATE_HOME");
	const char *home  = getenv("HOME");

	if (state && *state)
		snprintf(out, sz, "%s/awm/launcher_history", state);
	else if (home && *home)
		snprintf(out, sz, "%s/.local/state/awm/launcher_history", home);
	else
		snprintf(out, sz, "/tmp/awm_launcher_history");
}

static void
launcher_history_load(Launcher *launcher)
{
	FILE         *f;
	char          line[512];
	char         *tab;
	LauncherItem *item;

	f = fopen(launcher->history_path, "r");
	if (!f)
		return;

	while (fgets(line, sizeof(line), f)) {
		size_t len = strlen(line);
		while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
			line[--len] = '\0';

		tab = strchr(line, '\t');
		if (!tab)
			continue;
		*tab      = '\0';
		int count = atoi(tab + 1);
		if (count <= 0)
			continue;

		for (item = launcher->items; item; item = item->next) {
			if (strcmp(item->name, line) == 0) {
				item->launch_count = count;
				break;
			}
		}
	}
	fclose(f);
}

static void
launcher_history_save(Launcher *launcher)
{
	FILE         *f;
	LauncherItem *item;
	char          tmp[512];
	int           n;

	n = snprintf(tmp, sizeof(tmp), "%s", launcher->history_path);
	while (n > 0 && tmp[n - 1] != '/')
		n--;
	if (n > 0) {
		tmp[n] = '\0';
		for (int i = 1; i <= n; i++) {
			if (tmp[i] == '/' || tmp[i] == '\0') {
				char save = tmp[i];
				tmp[i]    = '\0';
				mkdir(tmp, 0700);
				tmp[i] = save;
			}
		}
	}

	f = fopen(launcher->history_path, "w");
	if (!f)
		return;

	for (item = launcher->items; item; item = item->next) {
		if (item->launch_count > 0)
			fprintf(f, "%s\t%d\n", item->name, item->launch_count);
	}
	fclose(f);
}

/* -------------------------------------------------------------------------
 * Append items helper (unchanged)
 * ---------------------------------------------------------------------- */

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

/* -------------------------------------------------------------------------
 * GTK UI helpers
 * ---------------------------------------------------------------------- */

/* Retrieve the LauncherItem* stored on a GtkListBoxRow */
static LauncherItem *
row_get_item(GtkListBoxRow *row)
{
	return (LauncherItem *) g_object_get_data(G_OBJECT(row), "launcher-item");
}

/* Build all rows for the listbox from the full item list.
 * Sorting: build a temporary pointer array, qsort it with launcher_item_cmp,
 * then append rows in that order.  Filtering is done via
 * gtk_list_box_set_filter_func — GTK calls it on every row automatically. */
static void
launcher_populate_listbox(Launcher *launcher)
{
	/* Count items */
	int           count = 0;
	LauncherItem *it;

	for (it = launcher->items; it; it = it->next)
		count++;

	if (count == 0)
		return;

	/* Build sorted pointer array */
	LauncherItem **arr = ecalloc(count, sizeof(LauncherItem *));
	int            idx = 0;
	for (it = launcher->items; it; it = it->next)
		arr[idx++] = it;
	qsort(arr, count, sizeof(LauncherItem *), launcher_item_cmp);

	/* Add rows */
	for (int i = 0; i < count; i++) {
		LauncherItem *item = arr[i];

		GtkWidget *row_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);

		/* Icon */
		if (item->icon) {
			GdkPixbuf *pb = gdk_pixbuf_get_from_surface(
			    item->icon, 0, 0, LAUNCHER_ICON_SIZE, LAUNCHER_ICON_SIZE);
			if (pb) {
				GtkWidget *img = gtk_image_new_from_pixbuf(pb);
				g_object_unref(pb);
				gtk_box_pack_start(GTK_BOX(row_box), img, FALSE, FALSE, 2);
			}
		}

		/* Label */
		GtkWidget *lbl = gtk_label_new(item->name);
		gtk_label_set_xalign(GTK_LABEL(lbl), 0.0f);
		gtk_box_pack_start(GTK_BOX(row_box), lbl, TRUE, TRUE, 0);

		GtkWidget *row = gtk_list_box_row_new();
		gtk_container_add(GTK_CONTAINER(row), row_box);
		g_object_set_data(G_OBJECT(row), "launcher-item", item);

		gtk_list_box_insert(GTK_LIST_BOX(launcher->listbox), row, -1);
	}

	free(arr);
	gtk_widget_show_all(launcher->listbox);
}

/* GtkListBoxFilterFunc — called by GTK for every row on invalidation */
static gboolean
launcher_filter_func(GtkListBoxRow *row, gpointer user_data)
{
	Launcher     *launcher = (Launcher *) user_data;
	LauncherItem *item     = row_get_item(row);
	const char   *text     = gtk_entry_get_text(GTK_ENTRY(launcher->search));

	if (!item)
		return FALSE;
	return launcher_item_matches(item, text);
}

/* GtkListBoxSortFunc */
static gint
launcher_sort_func(GtkListBoxRow *a, GtkListBoxRow *b, gpointer user_data)
{
	(void) user_data;
	LauncherItem *ia = row_get_item(a);
	LauncherItem *ib = row_get_item(b);

	if (!ia || !ib)
		return 0;

	/* Wrap in pointer so we can reuse launcher_item_cmp */
	const LauncherItem *pa = ia;
	const LauncherItem *pb = ib;
	return launcher_item_cmp(&pa, &pb);
}

/* Launch the currently selected row */
static void
launcher_launch_row(Launcher *launcher, LauncherItem *item)
{
	if (!item || !item->exec)
		return;

	item->launch_count++;
	launcher_history_save(launcher);

	launcher_hide(launcher);

	pid_t pid = fork();
	if (pid < 0) {
		awm_error("launcher: fork failed: %s", strerror(errno));
		return;
	}
	if (pid == 0) {
		signal(SIGCHLD, SIG_DFL);
		if (launcher->xc)
			close(xcb_get_file_descriptor(launcher->xc));
		setsid();
		if (item->terminal) {
			const char *term = launcher->terminal;
			if (!term || !*term)
				term = getenv("TERMINAL");
			if (!term || !*term)
				term = "st";
			execlp(term, term, "-e", "sh", "-c", item->exec, NULL);
		} else {
			execlp("sh", "sh", "-c", item->exec, NULL);
		}
		exit(1);
	}
}

/* Signal: row activated (double-click or Enter on a row) */
static void
on_row_activated(GtkListBox *listbox, GtkListBoxRow *row, gpointer user_data)
{
	Launcher     *launcher = (Launcher *) user_data;
	LauncherItem *item     = row_get_item(row);
	(void) listbox;

	launcher_launch_row(launcher, item);
}

/* Signal: search text changed — re-filter and re-sort rows */
static void
on_search_changed(GtkSearchEntry *entry, gpointer user_data)
{
	Launcher *launcher = (Launcher *) user_data;
	(void) entry;

	gtk_list_box_invalidate_filter(GTK_LIST_BOX(launcher->listbox));
	gtk_list_box_invalidate_sort(GTK_LIST_BOX(launcher->listbox));

	/* Select first visible row */
	GtkListBoxRow *first =
	    gtk_list_box_get_row_at_index(GTK_LIST_BOX(launcher->listbox), 0);
	/* Iterate to find first visible row */
	int i = 0;
	while (first && !gtk_widget_get_visible(GTK_WIDGET(first)))
		first = gtk_list_box_get_row_at_index(
		    GTK_LIST_BOX(launcher->listbox), ++i);
	if (first)
		gtk_list_box_select_row(GTK_LIST_BOX(launcher->listbox), first);
}

/* Signal: realize — set override-redirect so the WM does not manage this
 * window.  This must be done on the underlying GdkWindow after it is
 * created but before it is mapped. */
static void
on_window_realize(GtkWidget *widget, gpointer user_data)
{
	(void) user_data;
	GdkWindow *gdk_win = gtk_widget_get_window(widget);
	if (gdk_win)
		gdk_window_set_override_redirect(gdk_win, TRUE);
}

/* Signal: delete-event — hide instead of destroying the window */
static gboolean
on_delete_event(GtkWidget *widget, GdkEvent *event, gpointer user_data)
{
	Launcher *launcher = (Launcher *) user_data;
	(void) widget;
	(void) event;

	launcher_hide(launcher);
	return TRUE; /* prevent GTK from destroying the window */
}

/* Signal: key press on the window */
static gboolean
on_key_press(GtkWidget *widget, GdkEventKey *event, gpointer user_data)
{
	Launcher      *launcher = (Launcher *) user_data;
	GtkListBox    *lb       = GTK_LIST_BOX(launcher->listbox);
	GtkListBoxRow *sel;
	int            new_idx;
	(void) widget;

	switch (event->keyval) {
	case GDK_KEY_Escape:
		launcher_hide(launcher);
		return TRUE;

	case GDK_KEY_Return:
	case GDK_KEY_KP_Enter: {
		sel = gtk_list_box_get_selected_row(lb);
		if (sel)
			launcher_launch_row(launcher, row_get_item(sel));
		return TRUE;
	}

	case GDK_KEY_Up: {
		sel = gtk_list_box_get_selected_row(lb);
		if (!sel) {
			/* select last visible */
			int            i = 0;
			GtkListBoxRow *r, *last_vis = NULL;
			while ((r = gtk_list_box_get_row_at_index(lb, i++))) {
				if (gtk_widget_get_visible(GTK_WIDGET(r)))
					last_vis = r;
			}
			if (last_vis)
				gtk_list_box_select_row(lb, last_vis);
		} else {
			new_idx = gtk_list_box_row_get_index(sel) - 1;
			while (new_idx >= 0) {
				GtkListBoxRow *r = gtk_list_box_get_row_at_index(lb, new_idx);
				if (r && gtk_widget_get_visible(GTK_WIDGET(r))) {
					gtk_list_box_select_row(lb, r);
					GtkWidget *sw = gtk_widget_get_parent(
					    gtk_widget_get_parent(launcher->listbox));
					gtk_widget_activate(GTK_WIDGET(r));
					/* scroll into view */
					GtkAdjustment *adj = gtk_scrolled_window_get_vadjustment(
					    GTK_SCROLLED_WINDOW(sw));
					GtkAllocation alloc;
					gtk_widget_get_allocation(GTK_WIDGET(r), &alloc);
					gtk_adjustment_clamp_page(
					    adj, alloc.y, alloc.y + alloc.height);
					break;
				}
				new_idx--;
			}
		}
		return TRUE;
	}

	case GDK_KEY_Down: {
		sel = gtk_list_box_get_selected_row(lb);
		if (!sel) {
			/* select first visible */
			int            i = 0;
			GtkListBoxRow *r;
			while ((r = gtk_list_box_get_row_at_index(lb, i++))) {
				if (gtk_widget_get_visible(GTK_WIDGET(r))) {
					gtk_list_box_select_row(lb, r);
					break;
				}
			}
		} else {
			new_idx = gtk_list_box_row_get_index(sel) + 1;
			while (TRUE) {
				GtkListBoxRow *r = gtk_list_box_get_row_at_index(lb, new_idx);
				if (!r)
					break;
				if (gtk_widget_get_visible(GTK_WIDGET(r))) {
					gtk_list_box_select_row(lb, r);
					GtkWidget *sw = gtk_widget_get_parent(
					    gtk_widget_get_parent(launcher->listbox));
					GtkAdjustment *adj = gtk_scrolled_window_get_vadjustment(
					    GTK_SCROLLED_WINDOW(sw));
					GtkAllocation alloc;
					gtk_widget_get_allocation(GTK_WIDGET(r), &alloc);
					gtk_adjustment_clamp_page(
					    adj, alloc.y, alloc.y + alloc.height);
					break;
				}
				new_idx++;
			}
		}
		return TRUE;
	}

	default:
		break;
	}

	return FALSE;
}

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

Launcher *
launcher_create(xcb_connection_t *xc, xcb_window_t root, Clr **scheme,
    const char **fonts, size_t fontcount, const char *term)
{
	Launcher *launcher;
	char     *home;
	char      path[512];
	int       i;

	/* Suppress unused-parameter warnings — API compat only */
	(void) root;
	(void) scheme;
	(void) fonts;
	(void) fontcount;

	launcher           = ecalloc(1, sizeof(Launcher));
	launcher->xc       = xc;
	launcher->terminal = (term && *term) ? term : "st";

	/* Resolve history path */
	launcher_history_path(
	    launcher->history_path, sizeof(launcher->history_path));

	/* Scan desktop files */
	home = getenv("HOME");
	if (!home)
		home = "/root";

	for (i = 0; i < (int) (sizeof(desktop_paths) / sizeof(desktop_paths[0]));
	    i++) {
		if (desktop_paths[i] == NULL) {
			if (i == 2)
				snprintf(
				    path, sizeof(path), "%s/.local/share/applications", home);
			else if (i == 3)
				snprintf(path, sizeof(path),
				    "%s/.local/share/flatpak/exports/share/applications",
				    home);
			else
				continue;
		} else {
			snprintf(path, sizeof(path), "%s", desktop_paths[i]);
		}

		LauncherItem *items = launcher_load_desktop_files(path);
		launcher_append_items(launcher, items);
	}

	LauncherItem *path_items = launcher_scan_path(launcher->items);
	launcher_append_items(launcher, path_items);

	launcher_history_load(launcher);

	/* ----- Build GTK widget tree ----- */
	launcher->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
	gtk_window_set_decorated(GTK_WINDOW(launcher->window), FALSE);
	gtk_window_set_skip_taskbar_hint(GTK_WINDOW(launcher->window), TRUE);
	gtk_window_set_skip_pager_hint(GTK_WINDOW(launcher->window), TRUE);
	gtk_window_set_type_hint(
	    GTK_WINDOW(launcher->window), GDK_WINDOW_TYPE_HINT_POPUP_MENU);
	gtk_window_set_default_size(GTK_WINDOW(launcher->window), 420, 400);
	gtk_window_set_resizable(GTK_WINDOW(launcher->window), FALSE);

	GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_container_add(GTK_CONTAINER(launcher->window), vbox);

	launcher->search = gtk_search_entry_new();
	gtk_box_pack_start(GTK_BOX(vbox), launcher->search, FALSE, FALSE, 4);

	GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
	gtk_scrolled_window_set_policy(
	    GTK_SCROLLED_WINDOW(sw), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
	gtk_widget_set_size_request(sw, -1, 360);
	gtk_box_pack_start(GTK_BOX(vbox), sw, TRUE, TRUE, 0);

	launcher->listbox = gtk_list_box_new();
	gtk_list_box_set_selection_mode(
	    GTK_LIST_BOX(launcher->listbox), GTK_SELECTION_SINGLE);
	gtk_list_box_set_filter_func(
	    GTK_LIST_BOX(launcher->listbox), launcher_filter_func, launcher, NULL);
	gtk_list_box_set_sort_func(
	    GTK_LIST_BOX(launcher->listbox), launcher_sort_func, NULL, NULL);
	gtk_container_add(GTK_CONTAINER(sw), launcher->listbox);

	/* Populate rows once at startup */
	launcher_populate_listbox(launcher);

	/* Signals */
	g_signal_connect(launcher->search, "search-changed",
	    G_CALLBACK(on_search_changed), launcher);
	g_signal_connect(launcher->listbox, "row-activated",
	    G_CALLBACK(on_row_activated), launcher);
	g_signal_connect(
	    launcher->window, "realize", G_CALLBACK(on_window_realize), NULL);
	g_signal_connect(launcher->window, "key-press-event",
	    G_CALLBACK(on_key_press), launcher);
	g_signal_connect(launcher->window, "delete-event",
	    G_CALLBACK(on_delete_event), launcher);

	/* Hide by default */
	gtk_widget_hide(launcher->window);

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

	if (launcher->window) {
		gtk_widget_destroy(launcher->window);
		launcher->window  = NULL;
		launcher->search  = NULL;
		launcher->listbox = NULL;
	}

	for (item = launcher->items; item; item = next) {
		next = item->next;
		free(item->name);
		free(item->exec);
		free(item->icon_name);
		if (item->icon)
			cairo_surface_destroy(item->icon);
		free(item);
	}

	free(launcher);
	icon_alias_free();
}

void
launcher_show(Launcher *launcher, int x, int y)
{
	if (!launcher)
		return;

	/* Clear search text — triggers on_search_changed which re-filters */
	gtk_entry_set_text(GTK_ENTRY(launcher->search), "");

	/* Select first visible row */
	GtkListBoxRow *first =
	    gtk_list_box_get_row_at_index(GTK_LIST_BOX(launcher->listbox), 0);
	int fi = 0;
	while (first && !gtk_widget_get_visible(GTK_WIDGET(first)))
		first = gtk_list_box_get_row_at_index(
		    GTK_LIST_BOX(launcher->listbox), ++fi);
	if (first)
		gtk_list_box_select_row(GTK_LIST_BOX(launcher->listbox), first);

	/* Realize first so gtk_window_move takes effect before mapping.
	 * Also set override-redirect here defensively — the 'realize' signal
	 * fires only once; if GTK already realized the window internally (e.g.
	 * during gtk_widget_hide at create time) the signal callback is a no-op
	 * for subsequent show calls, so we must apply it unconditionally. */
	gtk_widget_realize(launcher->window);
	{
		GdkWindow *gdk_win = gtk_widget_get_window(launcher->window);
		if (gdk_win)
			gdk_window_set_override_redirect(gdk_win, TRUE);
	}
	gtk_window_move(GTK_WINDOW(launcher->window), x, y);
	gtk_widget_show_all(launcher->window);
	gtk_window_present(GTK_WINDOW(launcher->window));

	/* Give keyboard focus to the search entry */
	gtk_widget_grab_focus(launcher->search);

	launcher->visible = 1;
}

void
launcher_hide(Launcher *launcher)
{
	if (!launcher || !launcher->visible)
		return;

	gtk_widget_hide(launcher->window);
	launcher->visible = 0;
}

/* GTK handles all input through its own event loop.  Return 1 when visible
 * so that awm.c's keybinding handler knows to swallow the triggering event
 * and not re-dispatch it as a WM shortcut. */
int
launcher_handle_event(Launcher *launcher, xcb_generic_event_t *ev)
{
	(void) ev;

	if (!launcher || !launcher->visible)
		return 0;

	return 1;
}

/* Launch the currently selected row (called from awm.c if needed) */
void
launcher_launch_selected(Launcher *launcher)
{
	if (!launcher)
		return;

	GtkListBoxRow *row =
	    gtk_list_box_get_selected_row(GTK_LIST_BOX(launcher->listbox));
	if (!row)
		return;

	launcher_launch_row(launcher, row_get_item(row));
}
