/* AndrathWM - Xresources loading
 * See LICENSE file for copyright and license details. */

#ifndef XRDB_H
#define XRDB_H

#include "awm.h"

/* DPI value parsed from Xft.dpi in RESOURCE_MANAGER.
 * 0.0 means not found / not yet parsed. */
extern double xrdb_dpi;

void loadxrdb(void);
void xrdb(const Arg *arg);

#endif /* XRDB_H */
