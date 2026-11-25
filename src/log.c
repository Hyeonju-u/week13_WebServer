#include "log.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define LOG_LINE_MAX 1024

static log_level_t current_level = LOG_INFO;

static const char * find_level_name (log_level_t level);

/* ── 공개 함수 ── */

void set_log_level (log_level_t level) {
    current_level = level;
}

int parse_log_level (const char * name, log_level_t * level) {
    if (strcmp(name, "error") == 0) {
        *level = LOG_ERROR;
        return 0;
    }
    if (strcmp(name, "info") == 0) {
        *level = LOG_INFO;
        return 0;
    }
    if (strcmp(name, "debug") == 0) {
        *level = LOG_DEBUG;
        return 0;
    }
    return -1;
}

/* 메모리에 쌓지 않고 한 줄씩 바로 stderr로 내보낸다 (NFR-11) */
void write_log (log_level_t level, const char * format, ...) {
    if (level > current_level) {
        return;
    }
    char line[LOG_LINE_MAX];
    int prefix_length = snprintf(line, sizeof(line), "[%s] ",
        find_level_name(level));
    if (prefix_length < 0) {
        return;
    }
    size_t room = sizeof(line) - (size_t) prefix_length - 2;
    va_list arguments;
    va_start(arguments, format);
    int message_length = vsnprintf(line + prefix_length, room + 1, format,
        arguments);
    va_end(arguments);
    if (message_length < 0) {
        return;
    }
    size_t length = (size_t) prefix_length;
    length += (size_t) message_length < room ? (size_t) message_length : room;
    line[length] = '\n';
    line[length + 1] = '\0';
    fputs(line, stderr);
}

/* ── 도우미 함수 ── */

static const char * find_level_name (log_level_t level) {
    if (level == LOG_ERROR) {
        return "ERROR";
    }
    if (level == LOG_INFO) {
        return "INFO";
    }
    return "DEBUG";
}
