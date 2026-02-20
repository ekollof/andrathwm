/* AndrathWM - process spawning
 * See LICENSE file for copyright and license details. */

#include "spawn.h"
#include "awm.h"
#include "client.h"
#include "monitor.h"
#include "xrdb.h"
#include "config.h"

/* autostart script filenames */
static const char autostartblocksh[] = "autostart_blocking.sh";
static const char autostartsh[]      = "autostart.sh";
static const char awmdir[]           = "awm";
static const char localshare[]       = ".local/share";

void
runautostart(void)
{
	char       *pathpfx;
	char       *path;
	char       *xdgdatahome;
	char       *home;
	struct stat sb;

	if ((home = getenv("HOME")) == NULL)
		/* this is almost impossible */
		return;

	/* if $XDG_DATA_HOME is set and not empty, use $XDG_DATA_HOME/awm,
	 * otherwise use ~/.local/share/awm as autostart script directory
	 */
	xdgdatahome = getenv("XDG_DATA_HOME");
	if (xdgdatahome != NULL && *xdgdatahome != '\0') {
		/* space for path segments, separators and nul */
		pathpfx = ecalloc(1, strlen(xdgdatahome) + strlen(awmdir) + 2);

		if (snprintf(pathpfx, strlen(xdgdatahome) + strlen(awmdir) + 2,
		        "%s/%s", xdgdatahome, awmdir) < 0) {
			free(pathpfx);
			return;
		}
	} else {
		/* space for path segments, separators and nul */
		pathpfx =
		    ecalloc(1, strlen(home) + strlen(localshare) + strlen(awmdir) + 3);

		if (snprintf(pathpfx, strlen(home) + strlen(awmdir) + 3, "%s/.%s",
		        home, awmdir) < 0) {
			free(pathpfx);
			return;
		}
	}

	/* check if the autostart script directory exists */
	if (!(stat(pathpfx, &sb) == 0 && S_ISDIR(sb.st_mode))) {
		/* the XDG conformant path does not exist or is no directory
		 * so we try ~/.awm instead
		 */
		char *pathpfx_new =
		    realloc(pathpfx, strlen(home) + strlen(awmdir) + 3);
		if (pathpfx_new == NULL) {
			free(pathpfx);
			return;
		}
		pathpfx = pathpfx_new;

		if (snprintf(pathpfx, strlen(home) + strlen(awmdir) + 3, "%s/.%s",
		        home, awmdir) < 0) {
			free(pathpfx);
			return;
		}
	}

	/* try the blocking script first */
	path = ecalloc(1, strlen(pathpfx) + strlen(autostartblocksh) + 2);
	if (snprintf(path, strlen(pathpfx) + strlen(autostartblocksh) + 2, "%s/%s",
	        pathpfx, autostartblocksh) < 0) {
		free(path);
		free(pathpfx);
	}

	if (access(path, X_OK) == 0)
		system(path);

	/* now the non-blocking script */
	if (snprintf(path, strlen(pathpfx) + strlen(autostartsh) + 2, "%s/%s",
	        pathpfx, autostartsh) < 0) {
		free(path);
		free(pathpfx);
	}

	if (access(path, X_OK) == 0)
		system(strcat(path, " &"));

	free(pathpfx);
	free(path);
}

void
spawn(const Arg *arg)
{
	struct sigaction sa;

	if (arg->v == dmenucmd)
		dmenumon[0] = '0' + selmon->num;
	if (fork() == 0) {
		if (xc)
			close(xcb_get_file_descriptor(xc));
		setsid();

		sigemptyset(&sa.sa_mask);
		sa.sa_flags   = 0;
		sa.sa_handler = SIG_DFL;
		sigaction(SIGCHLD, &sa, NULL);

		execvp(((char **) arg->v)[0], (char **) arg->v);
		die("awm: execvp '%s' failed:", ((char **) arg->v)[0]);
	}
}

void
spawnscratch(const Arg *arg)
{
	if (fork() == 0) {
		if (xc)
			close(xcb_get_file_descriptor(xc));
		setsid();
		execvp(((char **) arg->v)[1], ((char **) arg->v) + 1);
		awm_error(
		    "execvp '%s' failed: %s", ((char **) arg->v)[1], strerror(errno));
		exit(EXIT_SUCCESS);
	}
}
