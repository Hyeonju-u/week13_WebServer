#ifndef PROXY_MESSAGE_H
#define PROXY_MESSAGE_H

#include <stddef.h>

#include "buffer.h"
#include "http_parser.h"

typedef struct {
    int status_code;
    size_t reason_offset;
    size_t reason_length;
    header_block_t headers;
    size_t head_length;
    int has_content_length;
    size_t content_length;
    int has_transfer_encoding;
} backend_response_t;

int build_forward_request (byte_buffer_t * forward,
    const http_request_t * request, const char * bytes,
    const char * client_address);
parse_result_t parse_backend_response (const char * bytes, size_t length,
    backend_response_t * response);
int append_relayed_head (byte_buffer_t * output, const char * bytes,
    const backend_response_t * response, int keep_alive);

#endif
