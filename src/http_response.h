#ifndef HTTP_RESPONSE_H
#define HTTP_RESPONSE_H

#include <stddef.h>

#include "buffer.h"

const char * get_status_text (int status_code);
int append_response_header (byte_buffer_t * output, int status_code,
    const char * content_type, size_t content_length, int keep_alive,
    const char * extra_headers);
int append_error_response (byte_buffer_t * output, int status_code,
    int keep_alive, int is_head, const char * extra_headers);
int append_continue_response (byte_buffer_t * output);

#endif
