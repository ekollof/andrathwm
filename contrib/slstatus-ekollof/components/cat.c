/* See LICENSE file for copyright and license details. */
#include <stdio.h>
#include <string.h>
#include <glob.h>

#include "../slstatus.h"
#include "../util.h"

const char *
cat(const char *path)
{
        char *f;
        FILE *fp;

        glob_t p;

        glob(path, GLOB_TILDE, NULL, &p);

        if (p.gl_pathc < 1) {
                warn("glob: No matches for '%s':", path);
                globfree(&p);
                return NULL;
        }

        if (!(fp = fopen(p.gl_pathv[0], "r"))) {
                warn("fopen '%s':", p.gl_pathv[0]);
                globfree(&p);
                return NULL;
        }

        f = fgets(buf, sizeof(buf) - 1, fp);

        if (fclose(fp) < 0) {
                warn("fclose '%s':", path);
                return NULL;
        }
        if (!f)
                return NULL;

        if ((f = strrchr(buf, '\n')))
                f[0] = '\0';

        globfree(&p);
        return buf[0] ? buf : NULL;
}

