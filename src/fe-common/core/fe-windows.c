/*
 windows.c : irssi

    Copyright (C) 1999-2000 Timo Sirainen

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/

#include "module.h"
#include "module-formats.h"
#include "modules.h"
#include "signals.h"
#include "commands.h"
#include "servers.h"
#include "misc.h"
#include "settings.h"

#include "levels.h"

#include "printtext.h"
#include "fe-windows.h"
#include "window-items.h"

GSList *windows; /* first in the list is the active window,
                    next is the last active, etc. */
WINDOW_REC *active_win;

static int daytag;
static int daycheck; /* 0 = don't check, 1 = time is 00:00, check,
                        2 = time is 00:00, already checked */

static int window_get_new_refnum(void)
{
	WINDOW_REC *win;
	GSList *tmp;
	int refnum;

	refnum = 1;
	tmp = windows;
	while (tmp != NULL) {
		win = tmp->data;

		if (refnum != win->refnum) {
			tmp = tmp->next;
			continue;
		}

		refnum++;
		tmp = windows;
	}

	return refnum;
}

WINDOW_REC *window_create(WI_ITEM_REC *item, int automatic)
{
	WINDOW_REC *rec;

	rec = g_new0(WINDOW_REC, 1);
	rec->refnum = window_get_new_refnum();

	windows = g_slist_prepend(windows, rec);
	signal_emit("window created", 2, rec, GINT_TO_POINTER(automatic));

	if (item != NULL) window_item_add(rec, item, automatic);
	if (windows->next == NULL || !automatic || settings_get_bool("window_auto_change")) {
		if (automatic && windows->next != NULL)
			signal_emit("window changed automatic", 1, rec);
		window_set_active(rec);
	}
	return rec;
}

/* removed_refnum was removed from the windows list, pack the windows so
   there won't be any holes. If there is any holes after removed_refnum,
   leave the windows behind it alone. */
static void windows_pack(int removed_refnum)
{
	WINDOW_REC *window;
	int refnum;

	for (refnum = removed_refnum+1;; refnum++) {
		window = window_find_refnum(refnum);
		if (window == NULL || window->sticky_refnum)
			break;

		window_set_refnum(window, refnum-1);
	}
}

void window_destroy(WINDOW_REC *window)
{
	g_return_if_fail(window != NULL);

	if (window->destroying) return;
	window->destroying = TRUE;
	windows = g_slist_remove(windows, window);

	if (active_win == window && windows != NULL) {
                active_win = NULL; /* it's corrupted */
		window_set_active(windows->data);
	}

	while (window->items != NULL)
		window_item_destroy(window->items->data);

        if (settings_get_bool("windows_auto_renumber"))
		windows_pack(window->refnum);

	signal_emit("window destroyed", 1, window);

	while (window->bound_items != NULL)
                window_bind_destroy(window, window->bound_items->data);

	g_free_not_null(window->hilight_color);
	g_free_not_null(window->servertag);
	g_free_not_null(window->theme_name);
	g_free_not_null(window->name);
	g_free(window);
}

void window_auto_destroy(WINDOW_REC *window)
{
	if (settings_get_bool("autoclose_windows") && windows->next != NULL &&
	    window->items == NULL && window->level == 0)
                window_destroy(window);
}

void window_set_active(WINDOW_REC *window)
{
	WINDOW_REC *old_window;

	if (window == active_win)
		return;

	old_window = active_win;
	active_win = window;
	if (active_win != NULL) {
		windows = g_slist_remove(windows, active_win);
		windows = g_slist_prepend(windows, active_win);
	}

        if (active_win != NULL)
		signal_emit("window changed", 2, active_win, old_window);
}

void window_change_server(WINDOW_REC *window, void *server)
{
	window->active_server = server;
	signal_emit("window server changed", 2, window, server);
}

void window_set_refnum(WINDOW_REC *window, int refnum)
{
	GSList *tmp;
	int old_refnum;

	g_return_if_fail(window != NULL);
	g_return_if_fail(refnum >= 1);
	if (window->refnum == refnum) return;

	for (tmp = windows; tmp != NULL; tmp = tmp->next) {
		WINDOW_REC *rec = tmp->data;

		if (rec->refnum == refnum) {
			rec->refnum = window->refnum;
                        signal_emit("window refnum changed", 2, rec, GINT_TO_POINTER(refnum));
			break;
		}
	}

	old_refnum = window->refnum;
	window->refnum = refnum;
	signal_emit("window refnum changed", 2, window, GINT_TO_POINTER(old_refnum));
}

void window_set_name(WINDOW_REC *window, const char *name)
{
	g_free_not_null(window->name);
	window->name = g_strdup(name);

	signal_emit("window name changed", 1, window);
}

void window_set_level(WINDOW_REC *window, int level)
{
	g_return_if_fail(window != NULL);

	window->level = level;
        signal_emit("window level changed", 1, window);
}

/* return active item's name, or if none is active, window's name */
char *window_get_active_name(WINDOW_REC *window)
{
	g_return_val_if_fail(window != NULL, NULL);

	if (window->active != NULL)
		return window->active->name;

	return window->name;
}

WINDOW_REC *window_find_level(void *server, int level)
{
	WINDOW_REC *match;
	GSList *tmp;

	match = NULL;
	for (tmp = windows; tmp != NULL; tmp = tmp->next) {
		WINDOW_REC *rec = tmp->data;

		if ((server == NULL || rec->active_server == server) &&
		    (rec->level & level)) {
			if (server == NULL || rec->active_server == server)
				return rec;
			match = rec;
		}
	}

	return match;
}

WINDOW_REC *window_find_closest(void *server, const char *name, int level)
{
	WINDOW_REC *window;
	WI_ITEM_REC *item;

	/* match by name */
	item = name == NULL ? NULL :
		window_item_find(server, name);
	if (item != NULL)
                return window_item_window(item);

	/* match by level */
	if (level != MSGLEVEL_HILIGHT)
		level &= ~(MSGLEVEL_HILIGHT | MSGLEVEL_NOHILIGHT);
	window = window_find_level(server, level);
	if (window != NULL) return window;

	/* match by level - ignore server */
	window = window_find_level(NULL, level);
	if (window != NULL) return window;

	/* fallback to active */
	return active_win;
}

WINDOW_REC *window_find_refnum(int refnum)
{
	GSList *tmp;

	for (tmp = windows; tmp != NULL; tmp = tmp->next) {
		WINDOW_REC *rec = tmp->data;

		if (rec->refnum == refnum)
			return rec;
	}

	return NULL;
}

WINDOW_REC *window_find_name(const char *name)
{
	GSList *tmp;

	g_return_val_if_fail(name != NULL, NULL);

	for (tmp = windows; tmp != NULL; tmp = tmp->next) {
		WINDOW_REC *rec = tmp->data;

		if (rec->name != NULL && g_strcasecmp(rec->name, name) == 0)
			return rec;
	}

	return NULL;
}

WINDOW_REC *window_find_item(SERVER_REC *server, const char *name)
{
	WINDOW_REC *rec;
	WI_ITEM_REC *item;

	g_return_val_if_fail(name != NULL, NULL);

	rec = window_find_name(name);
	if (rec != NULL) return rec;

	item = server == NULL ? NULL :
		window_item_find(server, name);
	if (item == NULL && server == NULL) {
		/* not found from the active server - any server? */
		item = window_item_find(NULL, name);
	}

	if (item == NULL) {
		char *chan;

		/* still nothing? maybe user just left the # in front of
		   channel, try again with it.. */
		chan = g_strdup_printf("#%s", name);
		item = server == NULL ? NULL :
			window_item_find(server, chan);
		if (item == NULL) item = window_item_find(NULL, chan);
		g_free(chan);
	}

	if (item == NULL)
		return 0;

	return window_item_window(item);
}

int window_refnum_prev(int refnum, int wrap)
{
	GSList *tmp;
	int prev, max;

	max = prev = -1;
	for (tmp = windows; tmp != NULL; tmp = tmp->next) {
		WINDOW_REC *rec = tmp->data;

		if (rec->refnum < refnum && (prev == -1 || rec->refnum > prev))
			prev = rec->refnum;
		if (wrap && (max == -1 || rec->refnum > max))
			max = rec->refnum;
	}

	return prev != -1 ? prev : max;
}

int window_refnum_next(int refnum, int wrap)
{
	GSList *tmp;
	int min, next;

	min = next = -1;
	for (tmp = windows; tmp != NULL; tmp = tmp->next) {
		WINDOW_REC *rec = tmp->data;

		if (rec->refnum > refnum && (next == -1 || rec->refnum < next))
			next = rec->refnum;
		if (wrap && (min == -1 || rec->refnum < min))
			min = rec->refnum;
	}

	return next != -1 ? next : min;
}

int windows_refnum_last(void)
{
	GSList *tmp;
	int max;

	max = -1;
	for (tmp = windows; tmp != NULL; tmp = tmp->next) {
		WINDOW_REC *rec = tmp->data;

		if (rec->refnum > max)
			max = rec->refnum;
	}

	return max;
}

static int window_refnum_cmp(WINDOW_REC *w1, WINDOW_REC *w2)
{
	return w1->refnum < w2->refnum ? -1 : 1;
}

GSList *windows_get_sorted(void)
{
	GSList *tmp, *sorted;

        sorted = NULL;
	for (tmp = windows; tmp != NULL; tmp = tmp->next) {
		WINDOW_REC *rec = tmp->data;

		sorted = g_slist_insert_sorted(sorted, rec, (GCompareFunc)
					       window_refnum_cmp);
	}

        return sorted;
}

WINDOW_BIND_REC *window_bind_add(WINDOW_REC *window, const char *servertag,
				 const char *name)
{
	WINDOW_BIND_REC *rec;

        g_return_val_if_fail(window != NULL, NULL);
        g_return_val_if_fail(servertag != NULL, NULL);
        g_return_val_if_fail(name != NULL, NULL);

	rec = g_new0(WINDOW_BIND_REC, 1);
        rec->name = g_strdup(name);
        rec->servertag = g_strdup(servertag);

	window->bound_items = g_slist_append(window->bound_items, rec);
        return rec;
}

void window_bind_destroy(WINDOW_REC *window, WINDOW_BIND_REC *rec)
{
	g_return_if_fail(window != NULL);
        g_return_if_fail(rec != NULL);

	window->bound_items = g_slist_remove(window->bound_items, rec);

        g_free(rec->servertag);
        g_free(rec->name);
        g_free(rec);
}

WINDOW_BIND_REC *window_bind_find(WINDOW_REC *window, const char *servertag,
				  const char *name)
{
	GSList *tmp;

        g_return_val_if_fail(window != NULL, NULL);
        g_return_val_if_fail(servertag != NULL, NULL);
        g_return_val_if_fail(name != NULL, NULL);

	for (tmp = window->bound_items; tmp != NULL; tmp = tmp->next) {
		WINDOW_BIND_REC *rec = tmp->data;

		if (g_strcasecmp(rec->name, name) == 0 &&
		    g_strcasecmp(rec->servertag, servertag) == 0)
                        return rec;
	}

        return NULL;
}

void window_bind_remove_unsticky(WINDOW_REC *window)
{
	GSList *tmp, *next;

	for (tmp = window->bound_items; tmp != NULL; tmp = next) {
		WINDOW_BIND_REC *rec = tmp->data;

		next = tmp->next;
		if (!rec->sticky)
                        window_bind_destroy(window, rec);
	}
}

static void sig_server_looking(SERVER_REC *server)
{
	GSList *tmp;

	g_return_if_fail(server != NULL);

	/* Try to keep some server assigned to windows..
	   Also change active window's server if the window is empty */
	for (tmp = windows; tmp != NULL; tmp = tmp->next) {
		WINDOW_REC *rec = tmp->data;

		if ((rec->servertag == NULL ||
		     g_strcasecmp(rec->servertag, server->tag) == 0) &&
		    (rec->active_server == NULL ||
		     (rec == active_win && rec->items == NULL)))
			window_change_server(rec, server);
	}
}

static void sig_server_disconnected(SERVER_REC *server)
{
	GSList *tmp;
        SERVER_REC *new_server;

	g_return_if_fail(server != NULL);

	new_server = servers == NULL ? NULL : servers->data;
	for (tmp = windows; tmp != NULL; tmp = tmp->next) {
		WINDOW_REC *rec = tmp->data;

		if (rec->active_server == server) {
			window_change_server(rec, rec->servertag != NULL ?
					     NULL : new_server);
		}
	}
}

static void sig_print_text(void)
{
	GSList *tmp;
	char month[100];
	time_t t;
	struct tm *tm;

	t = time(NULL);
	tm = localtime(&t);
	if (strftime(month, sizeof(month), "%b", tm) <= 0)
		month[0] = '\0';

	if (tm->tm_hour != 0 || tm->tm_min != 0)
		return;

	daycheck = 2;
	signal_remove("print text", (SIGNAL_FUNC) sig_print_text);

	/* day changed, print notice about it to every window */
	for (tmp = windows; tmp != NULL; tmp = tmp->next) {
		printformat_window(tmp->data, MSGLEVEL_NEVER, TXT_DAYCHANGE,
				   tm->tm_mday, tm->tm_mon+1,
				   1900+tm->tm_year, month);
	}
}

static int sig_check_daychange(void)
{
	time_t t;
	struct tm *tm;

	t = time(NULL);
	tm = localtime(&t);

	if (daycheck == 1 && tm->tm_hour == 0 && tm->tm_min == 0) {
		sig_print_text();
		return TRUE;
	}

	if (tm->tm_hour != 23 || tm->tm_min != 59) {
		daycheck = 0;
		return TRUE;
	}

	/* time is 23:59 */
	if (daycheck == 0) {
		daycheck = 1;
		signal_add("print text", (SIGNAL_FUNC) sig_print_text);
	}
	return TRUE;
}

static void read_settings(void)
{
	if (daytag != -1) {
		g_source_remove(daytag);
		daytag = -1;
	}

	if (settings_get_bool("timestamps"))
		daytag = g_timeout_add(30000, (GSourceFunc) sig_check_daychange, NULL);
}

void windows_init(void)
{
	active_win = NULL;
	daycheck = 0; daytag = -1;
	settings_add_bool("lookandfeel", "window_auto_change", FALSE);
	settings_add_bool("lookandfeel", "windows_auto_renumber", TRUE);

	read_settings();
	signal_add("server looking", (SIGNAL_FUNC) sig_server_looking);
	signal_add("server disconnected", (SIGNAL_FUNC) sig_server_disconnected);
	signal_add("server connect failed", (SIGNAL_FUNC) sig_server_disconnected);
	signal_add("setup changed", (SIGNAL_FUNC) read_settings);
}

void windows_deinit(void)
{
	if (daytag != -1) g_source_remove(daytag);
	if (daycheck == 1) signal_remove("print text", (SIGNAL_FUNC) sig_print_text);

	signal_remove("server looking", (SIGNAL_FUNC) sig_server_looking);
	signal_remove("server disconnected", (SIGNAL_FUNC) sig_server_disconnected);
	signal_remove("server connect failed", (SIGNAL_FUNC) sig_server_disconnected);
	signal_remove("setup changed", (SIGNAL_FUNC) read_settings);
}
