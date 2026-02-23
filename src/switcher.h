/* switcher.h — Alt+Tab / Super+Tab window switcher for awm
 * See LICENSE file for copyright and license details. */

#ifndef SWITCHER_H
#define SWITCHER_H

#include "awm.h"

/*
 * switcher_init() — create the persistent (hidden) GTK switcher window.
 * Call once from setup(), after gtk_init() and compositor_init().
 */
void switcher_init(void);

/*
 * switcher_show() — populate and show the switcher.
 *   arg->i == 0 : show clients on the current monitor only  (Alt+Tab)
 *   arg->i == 1 : show clients on all monitors              (Super+Tab)
 * Bound directly in config.h keys[].
 * If switcher is already active, behaves like switcher_next().
 */
void switcher_show(const Arg *arg);

/*
 * switcher_show_prev() — like switcher_show() but pre-selects the previous
 * entry (for Shift+Tab as the opening keystroke).
 *   arg->i == 0 : current monitor only
 *   arg->i == 1 : all monitors
 */
void switcher_show_prev(const Arg *arg);

/*
 * switcher_next() — advance selection one step forward.
 * No-op if switcher is not active.
 */
void switcher_next(const Arg *arg);

/*
 * switcher_prev() — advance selection one step backward.
 * No-op if switcher is not active.
 */
void switcher_prev(const Arg *arg);

/*
 * switcher_confirm_xkb() — confirm and hide the switcher (focus chosen
 * window). Called from the key-release handler on Alt/Super release.
 */
void switcher_confirm_xkb(const Arg *arg);

/*
 * switcher_cancel_xkb() — cancel the switcher without changing focus.
 * Called on Escape keypress.
 */
void switcher_cancel_xkb(const Arg *arg);

/*
 * switcher_cleanup() — destroy the GTK window and free all resources.
 * Call once from cleanup().
 */
void switcher_cleanup(void);

/*
 * switcher_active() — returns 1 while the switcher window is visible.
 * Used as a guard in enternotify() and focusin() to prevent awm stealing
 * focus while the user is cycling through windows.
 */
int switcher_active(void);

#endif /* SWITCHER_H */
