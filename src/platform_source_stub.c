/* See LICENSE file for copyright and license details. */

#include "platform_source.h"

guint
platform_source_attach(xcb_connection_t *xc, GMainContext *ctx,
    GSourceFunc callback, gpointer user_data)
{
	(void) xc;
	(void) ctx;
	(void) callback;
	(void) user_data;
	return 0;
}

void
platform_source_use_gtk_main_quit(void)
{
}
