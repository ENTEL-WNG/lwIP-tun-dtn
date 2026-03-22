#include "dtn_logger.h"
#include "dtn_config.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

// Internal state
static dtn_level_t global_log_level = DTN_LEVEL_INFO;

// Map enum values to strings for printing
static const char* level_strings[] = {
    "FATAL",
    "ERROR",
    "WARN ",
    "INFO ",
    "DEBUG"
};

void dtn_log_init() {

    if (dtn_config.DEBUG) {
        global_log_level = DTN_LEVEL_DEBUG;
    }

    DTN_INFO("Logger initialized (Level: %s)", level_strings[global_log_level]);
}

void dtn_log_write(dtn_level_t level, const char *file, int line, const char *fmt, ...) {
    if (level > global_log_level) {
        return;
    }

    char time_buffer[32];
    char user_buffer[1024];
    va_list args;

    // Timestamp
    struct timeval tv;
    gettimeofday(&tv, NULL);
    struct tm *tm_info = localtime(&tv.tv_sec);
    strftime(time_buffer, sizeof(time_buffer), "%Y-%m-%d %H:%M:%S", tm_info);

    // Format User Message
    va_start(args, fmt);
    vsnprintf(user_buffer, sizeof(user_buffer), fmt, args);
    va_end(args);

    // const char* color = "";
    // if (level <= LOG_ERR) color = "\033[1;31m";    // Red for errors
    // else if (level == LOG_WARNING) color = "\033[1;33m"; // Yellow
    // else if (level == LOG_INFO) color = "\033[1;32m";    // Green

    // fprintf(stderr, "%s[%s:%d]\033[0m %s\n", color, file, line, user_buffer);

    // Format: [Timestamp.ms] [LEVEL] [file:line] Message
    fprintf(stderr, "[%s.%03ld] [%s] [%s:%d] %s\n", 
            time_buffer, 
            (long)tv.tv_usec / 1000, 
            level_strings[level], 
            file, 
            line, 
            user_buffer);

    // fflush(stderr);
}