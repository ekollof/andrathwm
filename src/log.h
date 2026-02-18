/* See LICENSE file for copyright and license details. */
#ifndef LOG_H
#define LOG_H

/* Log levels */
typedef enum {
	LOG_LEVEL_DEBUG = 0,
	LOG_LEVEL_INFO,
	LOG_LEVEL_WARN,
	LOG_LEVEL_ERROR
} LogLevel;

/* Initialize logging subsystem */
void log_init(const char *ident);

/* Cleanup logging subsystem */
void log_cleanup(void);

/* Logging functions */
void log_debug(const char *func, int line, const char *fmt, ...);
void log_info(const char *func, int line, const char *fmt, ...);
void log_warn(const char *func, int line, const char *fmt, ...);
void log_error(const char *func, int line, const char *fmt, ...);

/* Convenience macros that automatically pass __func__ and __LINE__ */
#define awm_debug(...) log_debug(__func__, __LINE__, __VA_ARGS__)
#define awm_info(...) log_info(__func__, __LINE__, __VA_ARGS__)
#define awm_warn(...) log_warn(__func__, __LINE__, __VA_ARGS__)
#define awm_error(...) log_error(__func__, __LINE__, __VA_ARGS__)

#endif /* LOG_H */
