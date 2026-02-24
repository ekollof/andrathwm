/* See LICENSE file for copyright and license details.
 *
 * preview.h — window preview popup for awm-ui
 *
 * Displays live XComposite window thumbnails in a floating GTK popup.
 * Triggered by awm sending UI_MSG_PREVIEW_SHOW (bar hover or keybind).
 * The user can click a thumbnail to request focus; awm-ui sends
 * UI_MSG_PREVIEW_FOCUS and then UI_MSG_PREVIEW_DONE back to awm.
 */

#ifndef PREVIEW_H
#define PREVIEW_H

#include <stddef.h>
#include <stdint.h>

#include "ui_proto.h"

/* Initialise the preview module.
 * ui_send_fd is the socket fd to awm (for sending FOCUS / DONE messages).
 * Returns 0 on success, -1 on failure. */
int preview_init(int ui_send_fd);

/* Update the DPI used for preview geometry scaling.
 * Call whenever a UI_MSG_THEME message is received. */
void preview_update_theme(const UiThemePayload *t);

/* Show the preview popup.
 * entries points to count UiPreviewEntry structs read from the SHM segment.
 * anchor_x / anchor_y: screen coords of the bar hover trigger point. */
void preview_show(const UiPreviewEntry *entries, unsigned int count,
    int anchor_x, int anchor_y);

/* Hide and destroy the preview popup.
 * Sends UI_MSG_PREVIEW_DONE to awm so it can free snapshot pixmaps. */
void preview_hide(void);

/* Tear down module state. */
void preview_cleanup(void);

#endif /* PREVIEW_H */
