#ifndef HTTP_PARSER_H
#define HTTP_PARSER_H

#include <stddef.h>

#include "settings.h"

typedef enum {
    PARSE_COMPLETE,
    PARSE_NEED_MORE,
    PARSE_ERROR
} parse_result_t;

typedef enum {
    METHOD_GET,
    METHOD_HEAD,
    METHOD_POST
} http_method_t;

/* 헤더 한 줄의 위치. 버퍼를 옮기지 않는 동안만 유효한 오프셋이다 */
typedef struct {
    size_t name_offset;
    size_t name_length;
    size_t value_offset;
    size_t value_length;
} header_field_t;

typedef struct {
    header_field_t fields[HEADER_COUNT_MAX];
    size_t count;
    size_t end_offset;
} header_block_t;

typedef struct {
    http_method_t method;
    int version_minor;
    char path[REQUEST_LINE_MAX + 1];
    size_t uri_offset;
    size_t uri_length;
    size_t raw_path_length;
    header_block_t headers;
    int keep_alive;
    int expect_continue;
    int has_head;
    size_t head_length;
    size_t body_length;
    size_t total_length;
} http_request_t;

parse_result_t parse_request (const char * bytes, size_t length,
    http_request_t * request, int * status_code);
parse_result_t parse_header_block (const char * bytes, size_t length,
    size_t start, size_t max_bytes, header_block_t * block, int * status_code);
int find_header_index (const char * bytes, const header_block_t * block,
    const char * name, size_t first_index);
int parse_content_length (const char * bytes, const header_block_t * block,
    size_t * value);
int has_connection_token (const char * bytes, const header_block_t * block,
    const char * token);
int is_connection_listed (const char * bytes, const header_block_t * block,
    const char * name, size_t name_length);
int is_header_named (const char * bytes, const header_field_t * field,
    const char * name);

#endif
