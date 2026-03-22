#ifndef DTN_LOGGER_H
#define DTN_LOGGER_H

#include <syslog.h>

// Define custom levels
typedef enum {
    DTN_LEVEL_FATAL = 0,
    DTN_LEVEL_ERROR = 1,
    DTN_LEVEL_WARN  = 2,
    DTN_LEVEL_INFO  = 3,
    DTN_LEVEL_DEBUG = 4
} dtn_level_t;

void dtn_log_init(void);
void dtn_log_write(dtn_level_t level, const char *file, int line, const char *fmt, ...);

#define DTN_FATAL(fmt, ...) dtn_log_write(DTN_LEVEL_FATAL,  __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define DTN_ERROR(fmt, ...) dtn_log_write(DTN_LEVEL_ERROR,  __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define DTN_WARN(fmt, ...)  dtn_log_write(DTN_LEVEL_WARN,  __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define DTN_INFO(fmt, ...)  dtn_log_write(DTN_LEVEL_INFO,  __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define DTN_DEBUG(fmt, ...) dtn_log_write(DTN_LEVEL_DEBUG, __FILE__, __LINE__, fmt, ##__VA_ARGS__)

#endif