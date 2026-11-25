#ifndef LOG_H
#define LOG_H

typedef enum {
    LOG_ERROR = 0,
    LOG_INFO = 1,
    LOG_DEBUG = 2
} log_level_t;

void set_log_level (log_level_t level);
int parse_log_level (const char * name, log_level_t * level);
void write_log (log_level_t level, const char * format, ...);

#endif
