#include "http_parser.h"

#include <stdint.h>
#include <string.h>
#include <strings.h>

#define METHOD_LENGTH_MAX 16
#define NOT_FOUND ((size_t) -1)

typedef struct {
    const char * method;
    size_t method_length;
    const char * uri;
    size_t uri_length;
    const char * version;
    size_t version_length;
} request_line_parts_t;

static size_t find_request_line_end (const char * bytes, size_t length);
static int parse_request_line (const char * bytes, size_t line_length,
    http_request_t * request, int * status_code);
static int is_clean_line (const char * bytes, size_t length);
static int is_control_character (char character);
static int split_request_line (const char * bytes, size_t line_length,
    request_line_parts_t * parts);
static int fail_with (int * status_code, int value);
static int parse_method (const request_line_parts_t * parts,
    http_request_t * request, int * status_code);
static int is_token_character (char character);
static int parse_version (const request_line_parts_t * parts,
    http_request_t * request, int * status_code);
static int parse_uri (const char * bytes, const request_line_parts_t * parts,
    http_request_t * request, int * status_code);
static int decode_path (const char * raw, size_t raw_length,
    char * path_output);
static int read_encoded_byte (const char * text, size_t available);
static int read_hex_digit (char digit);
static int check_request_headers (const char * bytes, http_request_t * request,
    int * status_code);
static int check_host_header (const char * bytes,
    const http_request_t * request, int * status_code);
static int count_headers (const char * bytes, const header_block_t * block,
    const char * name);
static int read_body_length (const char * bytes, http_request_t * request,
    int * status_code);
static int check_transfer_encoding (const char * bytes,
    const http_request_t * request, int * status_code);
static void decide_keep_alive (const char * bytes, http_request_t * request);
static int check_expect_header (const char * bytes, http_request_t * request,
    int * status_code);
static int check_body_rules (const char * bytes, const http_request_t * request,
    int * status_code);
static size_t find_line_end (const char * bytes, size_t length,
    size_t position);
static int parse_header_line (const char * bytes, size_t line_start,
    size_t line_length, header_field_t * field);
static int is_blank_character (char character);
static int parse_digits (const char * digits, size_t length, size_t * value);
static int is_token_in_value (const char * value, size_t length,
    const char * token, size_t token_length);
static int is_token_equal (const char * piece, size_t piece_length,
    const char * token, size_t token_length);

/* ── 공개 함수 ── */

/*
 * 헤더가 완성되고 문제가 없으면 본문이 덜 왔어도 request를 채워 둔다
 * (has_head = 1). 서버가 100 Continue를 보낼 때를 알아야 하기 때문이다.
 * PARSE_COMPLETE는 본문까지 전부 도착했을 때만 돌려준다.
 * bytes는 \0으로 끝나지 않으므로 끝은 length로만 판단한다.
 */
parse_result_t parse_request (const char * bytes, size_t length,
    http_request_t * request, int * status_code) {
    /* 덜 온 데이터로 처음부터 다시 불리거나 keep-alive 다음 요청에 쓰여도
     * 이전 값이 섞이지 않게 한다 */
    memset(request, 0, sizeof(*request));
    size_t line_end = find_request_line_end(bytes, length);
    if (line_end == NOT_FOUND) {
        /* +2는 \r\n 몫이다. 줄 내용 8KB까지는 받아 준다 */
        if (length >= REQUEST_LINE_MAX + 2) {
            *status_code = 414;
            return PARSE_ERROR;
        }
        return PARSE_NEED_MORE;
    }
    if (parse_request_line(bytes, line_end, request, status_code) != 0) {
        return PARSE_ERROR;
    }
    parse_result_t result = parse_header_block(bytes, length, line_end + 2,
        HEADER_BYTES_MAX, &request->headers, status_code);
    if (result != PARSE_COMPLETE) {
        return result;
    }
    if (check_request_headers(bytes, request, status_code) != 0) {
        return PARSE_ERROR;
    }
    request->has_head = 1;
    request->head_length = request->headers.end_offset;
    request->total_length = request->head_length + request->body_length;
    if (length < request->total_length) {
        return PARSE_NEED_MORE;
    }
    /* 버퍼에 total_length보다 더 있으면 붙여 보낸 다음 요청이다 */
    return PARSE_COMPLETE;
}

/*
 * start부터 빈 줄까지 헤더를 읽는다. max_bytes는 줄바꿈을 포함한 상한이다.
 * 상한을 넘으면 431, 형식이 틀리면 400을 status_code에 넣는다.
 */
parse_result_t parse_header_block (const char * bytes, size_t length,
    size_t start, size_t max_bytes, header_block_t * block, int * status_code) {
    block->count = 0;
    size_t position = start;
    for (;;) {
        size_t line_end = find_line_end(bytes, length, position);
        if (line_end == NOT_FOUND) {
            if (length - position > max_bytes) {
                *status_code = 431;
                return PARSE_ERROR;
            }
            return PARSE_NEED_MORE;
        }
        if (line_end == position) {
            block->end_offset = position + 2;
            return PARSE_COMPLETE;
        }
        if (line_end + 2 - start > max_bytes
            || block->count == HEADER_COUNT_MAX) {
            *status_code = 431;
            return PARSE_ERROR;
        }
        header_field_t * field = &block->fields[block->count];
        if (parse_header_line(bytes, position, line_end - position, field)
            != 0) {
            *status_code = 400;
            return PARSE_ERROR;
        }
        block->count++;
        position = line_end + 2;
    }
}

/* first_index부터 name 헤더를 찾아 위치를 돌려준다. 없으면 -1 */
int find_header_index (const char * bytes, const header_block_t * block,
    const char * name, size_t first_index) {
    for (size_t i = first_index; i < block->count; i++) {
        if (is_header_named(bytes, &block->fields[i], name)) {
            return (int)i;
        }
    }
    return -1;
}

/*
 * 0: Content-Length 없음, 1: 있음(value에 담김), -1: 값이 잘못됨.
 * 같은 값이 여러 번 오는 것은 허용하고, 다른 값이 섞이면 -1이다.
 */
int parse_content_length (const char * bytes, const header_block_t * block,
    size_t * value) {
    int found = 0;
    int index = find_header_index(bytes, block, "Content-Length", 0);
    while (index >= 0) {
        const header_field_t * field = &block->fields[index];
        size_t current = 0;
        if (parse_digits(
                bytes + field->value_offset, field->value_length, &current)
            != 0) {
            return -1;
        }
        if (found && current != *value) {
            return -1;
        }
        *value = current;
        found = 1;
        index = find_header_index(
            bytes, block, "Content-Length", (size_t)index + 1);
    }
    return found;
}

int has_connection_token (const char * bytes, const header_block_t * block,
    const char * token) {
    return is_connection_listed(bytes, block, token, strlen(token));
}

/* Connection 헤더 값에 이름이 적힌 헤더는 중계할 때 지워야 한다 */
int is_connection_listed (const char * bytes, const header_block_t * block,
    const char * name, size_t name_length) {
    int index = find_header_index(bytes, block, "Connection", 0);
    while (index >= 0) {
        const header_field_t * field = &block->fields[index];
        if (is_token_in_value(bytes + field->value_offset, field->value_length,
                name, name_length)) {
            return 1;
        }
        index =
            find_header_index(bytes, block, "Connection", (size_t)index + 1);
    }
    return 0;
}

int is_header_named (const char * bytes, const header_field_t * field,
    const char * name) {
    if (strlen(name) != field->name_length) {
        return 0;
    }
    return strncasecmp(bytes + field->name_offset, name, field->name_length)
        == 0;
}

/* ── 도우미 함수 ── */

/*
 * 첫 \r\n의 \r 위치. 없으면 NOT_FOUND. 상한+2까지만 찾는다: 그 뒤에 \r\n이
 * 있어도 이미 너무 긴 줄이다.
 */
static size_t find_request_line_end (const char * bytes, size_t length) {
    size_t limit =
        length < REQUEST_LINE_MAX + 2 ? length : REQUEST_LINE_MAX + 2;
    for (size_t i = 0; i + 1 < limit; i++) {
        if (bytes[i] == '\r' && bytes[i + 1] == '\n') {
            return i;
        }
    }
    return NOT_FOUND;
}

static int parse_request_line (const char * bytes, size_t line_length,
    http_request_t * request, int * status_code) {
    request_line_parts_t parts;
    if (!is_clean_line(bytes, line_length)
        || split_request_line(bytes, line_length, &parts) != 0) {
        return fail_with(status_code, 400);
    }
    if (parse_method(&parts, request, status_code) != 0) {
        return -1;
    }
    if (parse_version(&parts, request, status_code) != 0) {
        return -1;
    }
    return parse_uri(bytes, &parts, request, status_code);
}

/* 줄 안의 제어 문자(\0, 홑 \r·\n 포함)는 요청 경계를 속이는 데 쓰인다 */
static int is_clean_line (const char * bytes, size_t length) {
    for (size_t i = 0; i < length; i++) {
        if (is_control_character(bytes[i])) {
            return 0;
        }
    }
    return 1;
}

static int is_control_character (char character) {
    unsigned char value = (unsigned char)character;
    return value < 0x20 || value == 0x7f;
}

static int split_request_line (const char * bytes, size_t line_length,
    request_line_parts_t * parts) {
    const char * first_space = memchr(bytes, ' ', line_length);
    if (first_space == NULL) {
        return -1;
    }
    const char * uri = first_space + 1;
    size_t after_method = line_length - (size_t)(uri - bytes);
    const char * second_space = memchr(uri, ' ', after_method);
    if (second_space == NULL) {
        return -1;
    }
    const char * version = second_space + 1;
    size_t after_uri = line_length - (size_t)(version - bytes);
    if (memchr(version, ' ', after_uri) != NULL) {
        return -1;
    }
    parts->method = bytes;
    parts->method_length = (size_t)(first_space - bytes);
    parts->uri = uri;
    parts->uri_length = (size_t)(second_space - uri);
    parts->version = version;
    parts->version_length = after_uri;
    if (parts->method_length == 0 || parts->uri_length == 0
        || parts->version_length == 0) {
        return -1;
    }
    return 0;
}

/* 항상 -1을 돌려줘서 오류 상태를 기록하면서 바로 return할 수 있다 */
static int fail_with (int * status_code, int value) {
    *status_code = value;
    return -1;
}

static int parse_method (const request_line_parts_t * parts,
    http_request_t * request, int * status_code) {
    if (parts->method_length > METHOD_LENGTH_MAX) {
        return fail_with(status_code, 400);
    }
    for (size_t i = 0; i < parts->method_length; i++) {
        if (!is_token_character(parts->method[i])) {
            return fail_with(status_code, 400);
        }
    }
    if (parts->method_length == 3 && memcmp(parts->method, "GET", 3) == 0) {
        request->method = METHOD_GET;
        return 0;
    }
    if (parts->method_length == 4 && memcmp(parts->method, "HEAD", 4) == 0) {
        request->method = METHOD_HEAD;
        return 0;
    }
    if (parts->method_length == 4 && memcmp(parts->method, "POST", 4) == 0) {
        request->method = METHOD_POST;
        return 0;
    }
    return fail_with(status_code, 501);
}

static int is_token_character (char character) {
    if ((character >= '0' && character <= '9')
        || (character >= 'A' && character <= 'Z')
        || (character >= 'a' && character <= 'z')) {
        return 1;
    }
    return character != '\0' && strchr("!#$%&'*+-.^_`|~", character) != NULL;
}

static int parse_version (const request_line_parts_t * parts,
    http_request_t * request, int * status_code) {
    const char * version = parts->version;
    if (parts->version_length != 8 || memcmp(version, "HTTP/", 5) != 0
        || version[5] < '0' || version[5] > '9' || version[6] != '.'
        || version[7] < '0' || version[7] > '9') {
        return fail_with(status_code, 400);
    }
    if (version[5] != '1' || (version[7] != '0' && version[7] != '1')) {
        return fail_with(status_code, 505);
    }
    request->version_minor = version[7] - '0';
    return 0;
}

static int parse_uri (const char * bytes, const request_line_parts_t * parts,
    http_request_t * request, int * status_code) {
    if (parts->uri[0] != '/') {
        return fail_with(status_code, 400);
    }
    const char * query = memchr(parts->uri, '?', parts->uri_length);
    size_t raw_path_length =
        query == NULL ? parts->uri_length : (size_t)(query - parts->uri);
    if (decode_path(parts->uri, raw_path_length, request->path) != 0) {
        return fail_with(status_code, 400);
    }
    request->uri_offset = (size_t)(parts->uri - bytes);
    request->uri_length = parts->uri_length;
    request->raw_path_length = raw_path_length;
    return 0;
}

/*
 * 경로만 정확히 한 번 디코딩한다 (REQ-11). 다시 디코딩하면 %252e가 .이 되어
 * 경로 탈출 검사를 우회할 수 있다. +는 공백으로 바꾸지 않는다.
 */
static int decode_path (const char * raw, size_t raw_length,
    char * path_output) {
    size_t length = 0;
    size_t i = 0;
    while (i < raw_length) {
        if (raw[i] != '%') {
            path_output[length++] = raw[i++];
            continue;
        }
        int value = read_encoded_byte(raw + i, raw_length - i);
        if (value < 0) {
            return -1;
        }
        path_output[length++] = (char)value;
        i += 3;
    }
    path_output[length] = '\0';
    return 0;
}

/* %XX 하나를 읽는다. 잘못된 16진수, %00, 인코딩된 / 와 \ 는 -1 */
static int read_encoded_byte (const char * text, size_t available) {
    if (available < 3) {
        return -1;
    }
    int high = read_hex_digit(text[1]);
    int low = read_hex_digit(text[2]);
    if (high < 0 || low < 0) {
        return -1;
    }
    int value = high * 16 + low;
    if (value == 0 || value == '/' || value == '\\') {
        return -1;
    }
    return value;
}

static int read_hex_digit (char digit) {
    if (digit >= '0' && digit <= '9') {
        return digit - '0';
    }
    if (digit >= 'a' && digit <= 'f') {
        return digit - 'a' + 10;
    }
    if (digit >= 'A' && digit <= 'F') {
        return digit - 'A' + 10;
    }
    return -1;
}

static int check_request_headers (const char * bytes, http_request_t * request,
    int * status_code) {
    if (check_host_header(bytes, request, status_code) != 0) {
        return -1;
    }
    if (read_body_length(bytes, request, status_code) != 0) {
        return -1;
    }
    if (check_transfer_encoding(bytes, request, status_code) != 0) {
        return -1;
    }
    decide_keep_alive(bytes, request);
    if (check_expect_header(bytes, request, status_code) != 0) {
        return -1;
    }
    return check_body_rules(bytes, request, status_code);
}

static int check_host_header (const char * bytes,
    const http_request_t * request, int * status_code) {
    if (request->version_minor == 0) {
        return 0;
    }
    if (count_headers(bytes, &request->headers, "Host") != 1) {
        return fail_with(status_code, 400);
    }
    int index = find_header_index(bytes, &request->headers, "Host", 0);
    if (request->headers.fields[index].value_length == 0) {
        return fail_with(status_code, 400);
    }
    return 0;
}

static int count_headers (const char * bytes, const header_block_t * block,
    const char * name) {
    int count = 0;
    int index = find_header_index(bytes, block, name, 0);
    while (index >= 0) {
        count++;
        index = find_header_index(bytes, block, name, (size_t)index + 1);
    }
    return count;
}

static int read_body_length (const char * bytes, http_request_t * request,
    int * status_code) {
    size_t length = 0;
    int result = parse_content_length(bytes, &request->headers, &length);
    if (result < 0) {
        return fail_with(status_code, 400);
    }
    request->body_length = result == 1 ? length : 0;
    return 0;
}

/* 둘이 함께 오면 프록시와 백엔드가 경계를 다르게 읽는다 (Smuggling) */
static int check_transfer_encoding (const char * bytes,
    const http_request_t * request, int * status_code) {
    if (find_header_index(bytes, &request->headers, "Transfer-Encoding", 0)
        < 0) {
        return 0;
    }
    if (find_header_index(bytes, &request->headers, "Content-Length", 0) >= 0) {
        return fail_with(status_code, 400);
    }
    return fail_with(status_code, 501);
}

static void decide_keep_alive (const char * bytes, http_request_t * request) {
    if (has_connection_token(bytes, &request->headers, "close")) {
        request->keep_alive = 0;
    } else if (has_connection_token(bytes, &request->headers, "keep-alive")) {
        request->keep_alive = 1;
    } else {
        request->keep_alive = request->version_minor == 1;
    }
}

static int check_expect_header (const char * bytes, http_request_t * request,
    int * status_code) {
    int index = find_header_index(bytes, &request->headers, "Expect", 0);
    if (index < 0) {
        return 0;
    }
    const header_field_t * field = &request->headers.fields[index];
    if (field->value_length != 12
        || strncasecmp(bytes + field->value_offset, "100-continue", 12) != 0) {
        return fail_with(status_code, 417);
    }
    request->expect_continue = 1;
    return 0;
}

static int check_body_rules (const char * bytes, const http_request_t * request,
    int * status_code) {
    if (request->method == METHOD_POST
        && find_header_index(bytes, &request->headers, "Content-Length", 0)
            < 0) {
        return fail_with(status_code, 411);
    }
    if (request->body_length > BODY_BYTES_MAX) {
        return fail_with(status_code, 413);
    }
    return 0;
}

/* position부터 처음 나오는 \r\n의 \r 위치. 없으면 NOT_FOUND */
static size_t find_line_end (const char * bytes, size_t length,
    size_t position) {
    for (size_t i = position; i + 1 < length; i++) {
        if (bytes[i] == '\r' && bytes[i + 1] == '\n') {
            return i;
        }
    }
    return NOT_FOUND;
}

static int parse_header_line (const char * bytes, size_t line_start,
    size_t line_length, header_field_t * field) {
    const char * line = bytes + line_start;
    if (is_blank_character(line[0])) {
        return -1;
    }
    for (size_t i = 0; i < line_length; i++) {
        if (is_control_character(line[i]) && line[i] != '\t') {
            return -1;
        }
    }
    const char * colon = memchr(line, ':', line_length);
    if (colon == NULL || colon == line) {
        return -1;
    }
    for (const char * cursor = line; cursor < colon; cursor++) {
        if (!is_token_character(*cursor)) {
            return -1;
        }
    }
    const char * value = colon + 1;
    const char * end = line + line_length;
    while (value < end && is_blank_character(*value)) {
        value++;
    }
    while (end > value && is_blank_character(end[-1])) {
        end--;
    }
    field->name_offset = line_start;
    field->name_length = (size_t)(colon - line);
    field->value_offset = line_start + (size_t)(value - line);
    field->value_length = (size_t)(end - value);
    return 0;
}

static int is_blank_character (char character) {
    return character == ' ' || character == '\t';
}

static int parse_digits (const char * digits, size_t length, size_t * value) {
    if (length == 0) {
        return -1;
    }
    size_t result = 0;
    for (size_t i = 0; i < length; i++) {
        if (digits[i] < '0' || digits[i] > '9') {
            return -1;
        }
        size_t digit = (size_t)(digits[i] - '0');
        if (result > (SIZE_MAX - digit) / 10) {
            return -1;
        }
        result = result * 10 + digit;
    }
    *value = result;
    return 0;
}

static int is_token_in_value (const char * value, size_t length,
    const char * token, size_t token_length) {
    size_t position = 0;
    while (position <= length) {
        const char * start = value + position;
        size_t remaining = length - position;
        const char * comma = memchr(start, ',', remaining);
        size_t piece_length =
            comma == NULL ? remaining : (size_t)(comma - start);
        if (is_token_equal(start, piece_length, token, token_length)) {
            return 1;
        }
        position += piece_length + 1;
    }
    return 0;
}

static int is_token_equal (const char * piece, size_t piece_length,
    const char * token, size_t token_length) {
    while (piece_length > 0 && is_blank_character(*piece)) {
        piece++;
        piece_length--;
    }
    while (piece_length > 0 && is_blank_character(piece[piece_length - 1])) {
        piece_length--;
    }
    return piece_length == token_length
        && strncasecmp(piece, token, token_length) == 0;
}
