/* AndrathWM - Pertag struct definition
 * See LICENSE file for copyright and license details.
 *
 * This header must be included AFTER awm.h (for Layout type) and AFTER
 * config.h has been included (for the tags[] array used by LENGTH(tags)).
 * Each .c file that needs access to Pertag fields should do:
 *
 *   #include "awm.h"
 *   #include "config.h"
 *   #include "pertag.h"
 */

#ifndef PERTAG_H
#define PERTAG_H

/* struct Pertag uses LENGTH(tags) which requires config.h to be included first
 */
struct Pertag {
	unsigned int curtag, prevtag;     /* current and previous tag */
	int   nmasters[LENGTH(tags) + 1]; /* number of windows in master area */
	float mfacts[LENGTH(tags) + 1];   /* mfacts per tag */
	unsigned int sellts[LENGTH(tags) + 1]; /* selected layouts */
	const Layout
	    *ltidxs[LENGTH(tags) + 1][2]; /* matrix of tags and layouts indexes */
	int  showbars[LENGTH(tags) + 1];  /* display bar for the current tag */
	int  drawwithgaps[LENGTH(tags) + 1]; /* gaps toggle for each tag */
	int  gappx[LENGTH(tags) + 1];        /* gaps for each tag */
};

#endif /* PERTAG_H */
