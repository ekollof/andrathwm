/* See LICENSE file for copyright and license details. */

#ifndef UTIL_H
#define UTIL_H

#include <stdnoreturn.h>
#include <unistd.h>

#ifndef MAX
#define MAX(A, B) ((A) > (B) ? (A) : (B))
#endif
#ifndef MIN
#define MIN(A, B) ((A) < (B) ? (A) : (B))
#endif
#define BETWEEN(X, A, B) ((A) <= (X) && (X) <= (B))

noreturn void die(const char *fmt, ...);
void         *ecalloc(size_t nmemb, size_t size);

/* Signal-safe logging - uses write() syscall, safe in signal handlers
 * NOTE: Only use string literals for prefix and msg (no formatting)
 */
#define LOG_SAFE(prefix, msg)                             \
	do {                                                  \
		write(STDERR_FILENO, "awm: ", 5);                 \
		write(STDERR_FILENO, prefix, sizeof(prefix) - 1); \
		write(STDERR_FILENO, ": ", 2);                    \
		write(STDERR_FILENO, msg, sizeof(msg) - 1);       \
		write(STDERR_FILENO, "\n", 1);                    \
	} while (0)

#define LOG_SAFE_ERR(msg) LOG_SAFE("error", msg)
#define LOG_SAFE_WARN(msg) LOG_SAFE("warning", msg)

#endif /* UTIL_H */
