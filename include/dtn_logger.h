#ifndef DTN_LOGGER_H
#define DTN_LOGGER_H

#include <syslog.h>

// Define custom levels
typedef enum {
    DTN_LOG_LEVEL_FATAL = 0,
    DTN_LOG_LEVEL_ERROR = 1,
    DTN_LOG_LEVEL_WARN = 2,
    DTN_LOG_LEVEL_INFO = 3,
    DTN_LOG_LEVEL_TEST = 4,
    DTN_LOG_LEVEL_DEBUG = 5
} dtn_log_level_t;

typedef enum { DATE = 0, SYS_TIME = 1 } dtn_time_t;

#include <stdbool.h>

void dtn_log_init(dtn_log_level_t level);
void dtn_log_write(dtn_log_level_t level, const char* file, int line, const char* fmt, ...);

// True if a message at @level would be emitted at the current log level. Lets the
// hot path skip building log-only strings (e.g. ip6addr_ntoa_r) when suppressed.
bool dtn_log_enabled(dtn_log_level_t level);

#define DTN_FATAL(fmt, ...) dtn_log_write(DTN_LOG_LEVEL_FATAL, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define DTN_ERROR(fmt, ...) dtn_log_write(DTN_LOG_LEVEL_ERROR, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define DTN_WARN(fmt, ...) dtn_log_write(DTN_LOG_LEVEL_WARN, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define DTN_INFO(fmt, ...) dtn_log_write(DTN_LOG_LEVEL_INFO, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define DTN_TEST(fmt, ...) dtn_log_write(DTN_LOG_LEVEL_TEST, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define DTN_DEBUG(fmt, ...) dtn_log_write(DTN_LOG_LEVEL_DEBUG, __FILE__, __LINE__, fmt, ##__VA_ARGS__)

#endif
