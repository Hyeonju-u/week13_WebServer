#ifndef BUFFER_H
#define BUFFER_H

#include <stddef.h>

typedef struct {
    char * data;
    size_t length;
    size_t capacity;
} byte_buffer_t;

void init_buffer (byte_buffer_t * buffer);
int reserve_buffer (byte_buffer_t * buffer, size_t extra_length);
int append_bytes (byte_buffer_t * buffer, const char * bytes, size_t length);
int append_format (byte_buffer_t * buffer, const char * format, ...);
void consume_bytes (byte_buffer_t * buffer, size_t length);
void release_buffer (byte_buffer_t * buffer);
void set_buffer_limit (size_t limit);
size_t get_total_buffer_bytes (void);

#endif
