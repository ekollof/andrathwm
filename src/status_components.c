/* AndrathWM - status components
 * See LICENSE file for copyright and license details. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "status_components.h"
#include "status_util.h"

#if defined(__linux__)
#include <stdint.h>
#include <unistd.h>
#endif

const char *
battery_status(const char *bat)
{
	static char result[64];
	const char *state;
	const char *perc;
	const char *icon;

	/* battery_state() returns a string literal, not status_buf — safe */
	state = battery_state(bat);
	/* battery_perc() writes to status_buf and returns it */
	perc = battery_perc(bat);
	if (!perc)
		return NULL;

	if (!state) {
		/* Copy out of status_buf before any further bprintf calls */
		snprintf(result, sizeof(result), "%s", perc);
		return result;
	}

	if (state[0] == '+')
		icon = "⚡";
	else if (state[0] == '-')
		icon = "🔋";
	else if (state[0] == 'o')
		icon = "🔌";
	else
		icon = "❓";

	/* Use our own buffer — perc points into status_buf, so status_bprintf
	 * would overwrite it while reading from it (UB). snprintf to result is safe. */
	snprintf(result, sizeof(result), "%s %s", icon, perc);
	return result;
}

#if defined(__linux__)
/*
 * https://www.kernel.org/doc/html/latest/power/power_supply_class.html
 */
#include <limits.h>

#define POWER_SUPPLY_CAPACITY "/sys/class/power_supply/%s/capacity"
#define POWER_SUPPLY_STATUS   "/sys/class/power_supply/%s/status"
#define POWER_SUPPLY_CHARGE   "/sys/class/power_supply/%s/charge_now"
#define POWER_SUPPLY_ENERGY   "/sys/class/power_supply/%s/energy_now"
#define POWER_SUPPLY_CURRENT  "/sys/class/power_supply/%s/current_now"
#define POWER_SUPPLY_POWER    "/sys/class/power_supply/%s/power_now"

static const char *
pick(const char *bat, const char *f1, const char *f2, char *path,
    size_t length)
{
	if (status_esnprintf(path, length, f1, bat) > 0 && access(path, R_OK) == 0)
		return f1;

	if (status_esnprintf(path, length, f2, bat) > 0 && access(path, R_OK) == 0)
		return f2;

	return NULL;
}

const char *
battery_perc(const char *bat)
{
	int  cap_perc;
	char path[PATH_MAX];

	if (status_esnprintf(path, sizeof(path), POWER_SUPPLY_CAPACITY, bat) < 0)
		return NULL;
	if (status_pscanf(path, "%d", &cap_perc) != 1)
		return NULL;

	return status_bprintf("%d", cap_perc);
}

const char *
battery_state(const char *bat)
{
	static struct {
		char *state;
		char *symbol;
	} map[] = {
		{ "Charging", "+" },
		{ "Discharging", "-" },
		{ "Full", "o" },
		{ "Not charging", "o" },
	};
	size_t i;
	char   path[PATH_MAX];
	char   state[12];

	if (status_esnprintf(path, sizeof(path), POWER_SUPPLY_STATUS, bat) < 0)
		return NULL;
	if (status_pscanf(path, "%12[a-zA-Z ]", state) != 1)
		return NULL;

	for (i = 0; i < STATUS_LEN(map); i++)
		if (!strcmp(map[i].state, state))
			break;

	return (i == STATUS_LEN(map)) ? "?" : map[i].symbol;
}

const char *
battery_remaining(const char *bat)
{
	uintmax_t charge_now, current_now, m, h;
	double    timeleft;
	char      path[PATH_MAX];
	char      state[12];

	if (status_esnprintf(path, sizeof(path), POWER_SUPPLY_STATUS, bat) < 0)
		return NULL;
	if (status_pscanf(path, "%12[a-zA-Z ]", state) != 1)
		return NULL;

	if (!pick(bat, POWER_SUPPLY_CHARGE, POWER_SUPPLY_ENERGY, path,
	        sizeof(path)) ||
	    status_pscanf(path, "%ju", &charge_now) < 0)
		return NULL;

	if (!strcmp(state, "Discharging")) {
		if (!pick(bat, POWER_SUPPLY_CURRENT, POWER_SUPPLY_POWER, path,
		        sizeof(path)) ||
		    status_pscanf(path, "%ju", &current_now) < 0)
			return NULL;

		if (current_now == 0)
			return NULL;

		timeleft = (double) charge_now / (double) current_now;
		h = timeleft;
		m = (timeleft - (double) h) * 60;

		return status_bprintf("%juh %jum", h, m);
	}

	return "";
}
#elif defined(__OpenBSD__)
#include <fcntl.h>
#include <machine/apmvar.h>
#include <sys/ioctl.h>
#include <unistd.h>

static int
load_apm_power_info(struct apm_power_info *apm_info)
{
	int fd;

	fd = open("/dev/apm", O_RDONLY);
	if (fd < 0) {
		status_warn("open '/dev/apm'");
		return 0;
	}

	memset(apm_info, 0, sizeof(struct apm_power_info));
	if (ioctl(fd, APM_IOC_GETPOWER, apm_info) < 0) {
		status_warn("ioctl APM_IOC_GETPOWER");
		close(fd);
		return 0;
	}
	close(fd);
	return 1;
}

const char *
battery_perc(const char *unused)
{
	struct apm_power_info apm_info;

	if (load_apm_power_info(&apm_info))
		return status_bprintf("%d", apm_info.battery_life);

	return NULL;
}

const char *
battery_state(const char *unused)
{
	struct {
		unsigned int state;
		char        *symbol;
	} map[] = {
		{ APM_AC_ON, "+" },
		{ APM_AC_OFF, "-" },
	};
	struct apm_power_info apm_info;
	size_t                i;

	if (load_apm_power_info(&apm_info)) {
		for (i = 0; i < STATUS_LEN(map); i++)
			if (map[i].state == apm_info.ac_state)
				break;

		return (i == STATUS_LEN(map)) ? "?" : map[i].symbol;
	}

	return NULL;
}

const char *
battery_remaining(const char *unused)
{
	struct apm_power_info apm_info;
	unsigned int          h, m;

	if (load_apm_power_info(&apm_info)) {
		if (apm_info.ac_state != APM_AC_ON) {
			h = apm_info.minutes_left / 60;
			m = apm_info.minutes_left % 60;
			return status_bprintf("%uh %02um", h, m);
		}
		return "";
	}

	return NULL;
}
#elif defined(__FreeBSD__)
#include <sys/sysctl.h>

#define BATTERY_LIFE  "hw.acpi.battery.life"
#define BATTERY_STATE "hw.acpi.battery.state"
#define BATTERY_TIME  "hw.acpi.battery.time"

const char *
battery_perc(const char *unused)
{
	int    cap_perc;
	size_t len;

	len = sizeof(cap_perc);
	if (sysctlbyname(BATTERY_LIFE, &cap_perc, &len, NULL, 0) < 0 || !len)
		return NULL;

	return status_bprintf("%d", cap_perc);
}

const char *
battery_state(const char *unused)
{
	int    state;
	size_t len;

	len = sizeof(state);
	if (sysctlbyname(BATTERY_STATE, &state, &len, NULL, 0) < 0 || !len)
		return NULL;

	switch (state) {
	case 0:
	case 2:
		return "+";
	case 1:
		return "-";
	default:
		return "?";
	}
}

const char *
battery_remaining(const char *unused)
{
	int    rem;
	size_t len;

	len = sizeof(rem);
	if (sysctlbyname(BATTERY_TIME, &rem, &len, NULL, 0) < 0 || !len || rem < 0)
		return NULL;

	return status_bprintf("%uh %02um", rem / 60, rem % 60);
}
#endif

#if defined(__linux__)
#define CPU_FREQ "/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq"

const char *
cpu_perc(const char *unused)
{
	static long double a[7];
	long double        b[7], sum;

	memcpy(b, a, sizeof(b));
	if (status_pscanf("/proc/stat", "%*s %Lf %Lf %Lf %Lf %Lf %Lf %Lf",
	        &a[0], &a[1], &a[2], &a[3], &a[4], &a[5], &a[6]) != 7)
		return NULL;

	if (b[0] == 0)
		return NULL;

	sum = (b[0] + b[1] + b[2] + b[3] + b[4] + b[5] + b[6]) -
	    (a[0] + a[1] + a[2] + a[3] + a[4] + a[5] + a[6]);

	if (sum == 0)
		return NULL;

	return status_bprintf("%d",
	    (int) (100 *
	        ((b[0] + b[1] + b[2] + b[5] + b[6]) -
	            (a[0] + a[1] + a[2] + a[5] + a[6])) /
	        sum));
}
#elif defined(__OpenBSD__)
#include <sys/param.h>
#include <sys/sched.h>
#include <sys/sysctl.h>

const char *
cpu_perc(const char *unused)
{
	int       mib[2];
	static uintmax_t a[CPUSTATES];
	uintmax_t b[CPUSTATES], sum;
	size_t    size;

	mib[0] = CTL_KERN;
	mib[1] = KERN_CPTIME;

	size = sizeof(a);
	memcpy(b, a, sizeof(b));
	if (sysctl(mib, 2, &a, &size, NULL, 0) < 0)
		return NULL;
	if (b[0] == 0)
		return NULL;

	sum = (a[CP_USER] + a[CP_NICE] + a[CP_SYS] + a[CP_INTR] + a[CP_IDLE]) -
	    (b[CP_USER] + b[CP_NICE] + b[CP_SYS] + b[CP_INTR] + b[CP_IDLE]);

	if (sum == 0)
		return NULL;

	return status_bprintf("%d",
	    100 *
	        ((a[CP_USER] + a[CP_NICE] + a[CP_SYS] + a[CP_INTR]) -
	            (b[CP_USER] + b[CP_NICE] + b[CP_SYS] + b[CP_INTR])) /
	        sum);
}
#elif defined(__FreeBSD__)
#include <sys/param.h>
#include <sys/sysctl.h>

const char *
cpu_perc(const char *unused)
{
	size_t size;
	static long a[CPUSTATES];
	long        b[CPUSTATES], sum;

	size = sizeof(a);
	memcpy(b, a, sizeof(b));
	if (sysctlbyname("kern.cp_time", &a, &size, NULL, 0) < 0 || !size)
		return NULL;
	if (b[0] == 0)
		return NULL;

	sum = (a[CP_USER] + a[CP_NICE] + a[CP_SYS] + a[CP_INTR] + a[CP_IDLE]) -
	    (b[CP_USER] + b[CP_NICE] + b[CP_SYS] + b[CP_INTR] + b[CP_IDLE]);

	if (sum == 0)
		return NULL;

	return status_bprintf("%d",
	    100 *
	        ((a[CP_USER] + a[CP_NICE] + a[CP_SYS] + a[CP_INTR]) -
	            (b[CP_USER] + b[CP_NICE] + b[CP_SYS] + b[CP_INTR])) /
	        sum);
}
#endif

const char *
datetime(const char *fmt)
{
	time_t t;

	t = time(NULL);
	if (!strftime(status_buf, sizeof(status_buf), fmt, localtime(&t))) {
		status_warn("strftime result exceeds buffer");
		return NULL;
	}

	return status_buf;
}

const char *
load_avg(const char *unused)
{
	double avgs[3];
	int whole, frac;

	if (getloadavg(avgs, 3) < 0) {
		status_warn("getloadavg failed");
		return NULL;
	}

	/* Use integer arithmetic to avoid locale-specific decimal separator */
	whole = (int)avgs[0];
	frac  = (int)((avgs[0] - whole) * 10 + 0.05);
	return status_bprintf("%d.%d", whole, frac);
}

#if defined(__linux__)
const char *
ram_total(const char *unused)
{
	uintmax_t total;

	if (status_pscanf("/proc/meminfo", "MemTotal: %ju kB\n", &total) != 1)
		return NULL;

	return status_fmt_human(total * 1024, 1024);
}

const char *
ram_used(const char *unused)
{
	uintmax_t total, free, buffers, cached, used;

	if (status_pscanf("/proc/meminfo",
	        "MemTotal: %ju kB\n"
	        "MemFree: %ju kB\n"
	        "MemAvailable: %ju kB\n"
	        "Buffers: %ju kB\n"
	        "Cached: %ju kB\n",
	        &total, &free, &buffers, &buffers, &cached) != 5)
		return NULL;

	used = (total - free - buffers - cached);
	return status_fmt_human(used * 1024, 1024);
}
#elif defined(__OpenBSD__)
#include <stdlib.h>
#include <sys/sysctl.h>
#include <sys/types.h>
#include <unistd.h>

#define LOG1024 10
#define pagetok(size, pageshift) (size_t) (size << (pageshift - LOG1024))

static int
load_uvmexp(struct uvmexp *uvmexp)
{
	int    uvmexp_mib[] = { CTL_VM, VM_UVMEXP };
	size_t size;

	size = sizeof(*uvmexp);
	if (sysctl(uvmexp_mib, 2, uvmexp, &size, NULL, 0) >= 0)
		return 1;

	return 0;
}

const char *
ram_total(const char *unused)
{
	struct uvmexp uvmexp;

	if (!load_uvmexp(&uvmexp))
		return NULL;

	return status_fmt_human(pagetok(uvmexp.npages, uvmexp.pageshift) * 1024,
	    1024);
}

const char *
ram_used(const char *unused)
{
	struct uvmexp uvmexp;

	if (!load_uvmexp(&uvmexp))
		return NULL;

	return status_fmt_human(pagetok(uvmexp.active, uvmexp.pageshift) * 1024,
	    1024);
}
#elif defined(__FreeBSD__)
#include <sys/sysctl.h>
#include <sys/vmmeter.h>
#include <unistd.h>
#include <vm/vm_param.h>

const char *
ram_total(const char *unused)
{
	unsigned int npages;
	size_t       len;

	len = sizeof(npages);
	if (sysctlbyname("vm.stats.vm.v_page_count", &npages, &len, NULL, 0) < 0 ||
	    !len)
		return NULL;

	return status_fmt_human(npages * getpagesize(), 1024);
}

const char *
ram_used(const char *unused)
{
	unsigned int active;
	size_t       len;

	len = sizeof(active);
	if (sysctlbyname("vm.stats.vm.v_active_count", &active, &len, NULL, 0) < 0 ||
	    !len)
		return NULL;

	return status_fmt_human(active * getpagesize(), 1024);
}
#endif

#if defined(CLOCK_BOOTTIME)
#define UPTIME_FLAG CLOCK_BOOTTIME
#elif defined(CLOCK_UPTIME)
#define UPTIME_FLAG CLOCK_UPTIME
#else
#define UPTIME_FLAG CLOCK_MONOTONIC
#endif

const char *
uptime(const char *unused)
{
	uintmax_t      h, m;
	struct timespec uptime_ts;

	if (clock_gettime(UPTIME_FLAG, &uptime_ts) < 0) {
		status_warn("clock_gettime failed");
		return NULL;
	}

	h = uptime_ts.tv_sec / 3600;
	m = uptime_ts.tv_sec % 3600 / 60;

	return status_bprintf("%juh %jum", h, m);
}
