/* See LICENSE file for copyright and license details. */
/* platform_x11.c — definition of the X11 platform context singleton.
 *
 * g_plat is zero-initialised at program start.  All fields are populated
 * by setup() in awm.c before any other module reads them.
 */

#include "awm.h"
#include "platform.h"

PlatformCtx g_plat;
