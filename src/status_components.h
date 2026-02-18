/* AndrathWM - status components
 * See LICENSE file for copyright and license details. */

#ifndef STATUS_COMPONENTS_H
#define STATUS_COMPONENTS_H

/* battery */
const char *battery_perc(const char *bat);
const char *battery_remaining(const char *bat);
const char *battery_state(const char *bat);
const char *battery_status(const char *bat);

/* cpu */
const char *cpu_perc(const char *unused);

/* datetime */
const char *datetime(const char *fmt);

/* load average */
const char *load_avg(const char *unused);

/* ram */
const char *ram_total(const char *unused);
const char *ram_used(const char *unused);

/* uptime */
const char *uptime(const char *unused);

#endif /* STATUS_COMPONENTS_H */
