#ifndef HTTP_H
#define HTTP_H

#include <stddef.h>

#define HEADER_LIMIT 16384
#define TARGET_LIMIT 4096
#define BODY_LIMIT (1024 * 1024)

typedef struct {
    const char * method;
    const char * path;
    const char * query;
    const char * content_type;
    size_t body_length;
} request_t;

size_t find_header_end(const char * data, size_t length, size_t start);
int parse_request_headers(char * headers, size_t length, request_t * request);

#endif
