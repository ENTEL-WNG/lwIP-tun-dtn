#include "dtn_logger.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "dtn_config.h"
#include "lwip/sys.h"

// Internal state
static dtn_level_t global_log_level = DTN_LEVEL_DEBUG;
static dtn_time_t global_time_format = SYS_TIME;

// Map enum values to strings for printing
static const char* level_strings[] = {"FATAL", "ERROR", "WARN ", "INFO ", "DEBUG"};

void dtn_log_init() {
    if (dtn_config.env == DTN_ENV_PRODUCTION) {
        global_log_level = DTN_LEVEL_INFO;
    }

    DTN_INFO("Logger initialized (Level: %s)", level_strings[global_log_level]);
}

void dtn_log_write(dtn_level_t level, const char* file, int line, const char* fmt, ...) {
    if (level > global_log_level) {
        return;
    }

    char user_buffer[1024];
    va_list args;

    if (global_time_format == SYS_TIME) {
        // Get time since boot in milliseconds
        u32_t now = sys_now();

        // Split into seconds and remaining milliseconds for readability
        u32_t secs = now / 1000;
        u32_t msecs = now % 1000;

        // Format User Message
        va_start(args, fmt);
        vsnprintf(user_buffer, sizeof(user_buffer), fmt, args);
        va_end(args);

        // Print format: [Seconds.Milliseconds]
        fprintf(stderr, "[%5u.%03u] [%s] [%s:%03d] %s\n", secs, msecs, level_strings[level], file,
                line, user_buffer);

        return;
    }

    char time_buffer[32];

    // Timestamp
    struct timeval tv;
    gettimeofday(&tv, NULL);
    struct tm* tm_info = localtime(&tv.tv_sec);
    strftime(time_buffer, sizeof(time_buffer), "%Y-%m-%d %H:%M:%S", tm_info);

    // Format User Message
    va_start(args, fmt);
    vsnprintf(user_buffer, sizeof(user_buffer), fmt, args);
    va_end(args);

    fprintf(stderr, "[%s.%06ld] [%s] [%s:%03d] %s\n", time_buffer, (long)tv.tv_usec,
            level_strings[level], file, line, user_buffer);

    // fflush(stderr);
}