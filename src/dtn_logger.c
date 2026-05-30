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
static dtn_log_level_t global_log_level = DTN_LOG_LEVEL_DEBUG;
static dtn_time_t global_time_format = SYS_TIME;

// Map enum values to strings for printing
static const char* level_strings[] = {"FATAL", "ERROR", "WARN ", "INFO ", "TEST ", "DEBUG"};

void dtn_log_init(dtn_log_level_t level) {
    global_log_level = level;
    DTN_INFO("Logger initialized (Level: %s)", level_strings[global_log_level]);
}

void dtn_log_write(dtn_log_level_t level, const char* file, int line, const char* fmt, ...) {
    if (level > global_log_level) {
        return;
    }

    // Strip leading ../ sequences so paths like ../src/dtn_storage.c become
    // src/dtn_storage.c, while keeping the meaningful directory component.
    const char* trimmed = file;
    while (trimmed[0] == '.' && trimmed[1] == '.' && trimmed[2] == '/')
        trimmed += 3;

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

        // %-20.20s: left-aligned, always exactly 20 chars (pad or truncate)
        fprintf(stderr, "[%5u.%03u] [%s] [%-20.20s:%4d] %s\n", secs, msecs, level_strings[level], trimmed, line, user_buffer);

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

    fprintf(stderr, "[%s.%06ld] [%s] [%-20.20s:%4d] %s\n", time_buffer, (long)tv.tv_usec, level_strings[level], trimmed, line, user_buffer);

    // fflush(stderr);
}