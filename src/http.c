#include "http.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

typedef struct {
    bool has_host;
    bool has_length;
    bool has_encoding;
    bool length_exceeded;
} fields_t;

static int parse_request_line(char * line, request_t * request);
static bool is_token(const char * value);
static int parse_field(char * line, fields_t * fields, request_t * request);
static bool is_valid_host(char * host);
static bool is_valid_port(const char * port);
static int parse_length(const char * value, fields_t * fields,
                        request_t * request);

/* ── 공개 함수 ── */

/* 헤더 끝 다음 위치를 반환한다. 0이면 더 받아야 한다. */
size_t find_header_end (const char * data, size_t length, size_t start) {
    for (size_t i = start; i + 4 <= length; i++) {
        if (memcmp(data + i, "\r\n\r\n", 4) == 0) {
            return i + 4;
        }
    }
    return 0;
}

/* 완성된 헤더를 제자리에서 나눈다. 요청 문자열은 헤더 버퍼를 빌린다.
 * 성공은 0, 실패는 응답에 사용할 HTTP 상태 코드다. */
int parse_request_headers (char * headers, size_t length, request_t * request) {
    if (length < 4 || memcmp(headers + length - 4, "\r\n\r\n", 4) != 0
        || memchr(headers, '\0', length) != NULL) {
        return 400;
    }
    headers[length - 2] = '\0';
    char * end = strstr(headers, "\r\n");
    if (end == NULL) {
        return 400;
    }
    *end = '\0';
    *request = (request_t){0};
    /* 22번 첫 줄에서 GET과 /sample.html을 분리하고 HTTP 버전을 확인한다. */
    int status = parse_request_line(headers, request);
    fields_t fields = {0};
    char * line = end + 2;
    while (status == 0 && *line != '\0') {
        end = strstr(line, "\r\n");
        if (end == NULL) {
            return 400;
        }
        *end = '\0';
        /* 23번 둘째 줄부터 한 줄씩 읽어 Host 등의 헤더 필드를 처리한다. */
        status = parse_field(line, &fields, request);
        line = end + 2;
    }
    if (status != 0) {
        return status;
    }
    /* 마지막 줄까지 읽은 뒤에야 Host가 빠졌는지 알 수 있다. */
    if (!fields.has_host || (fields.has_encoding && fields.has_length)) {
        return 400;
    }
    if (fields.has_encoding) {
        return 501;
    }
    if (fields.length_exceeded) {
        return 413;
    }
    if (strcmp(request->method, "POST") == 0 && !fields.has_length) {
        return 411;
    }
    return 0;
}

/* ── 도우미 함수 ── */

static int parse_request_line (char * line, request_t * request) {
    char * target = strchr(line, ' ');
    if (target == NULL) {
        return 400;
    }
    *target++ = '\0';
    char * version = strchr(target, ' ');
    if (version == NULL || *target != '/' || !is_token(line)) {
        return 400;
    }
    *version++ = '\0';
    if (strcmp(version, "HTTP/1.1") != 0 || strchr(target, '#') != NULL) {
        return 400;
    }
    if (strlen(target) > TARGET_LIMIT) {
        return 414;
    }
    for (const unsigned char * byte = (void *)target; *byte; byte++) {
        if (*byte <= 32 || *byte == 127) {
            return 400;
        }
    }
    char * query = strchr(target, '?');
    if (query != NULL) {
        *query++ = '\0';
    }
    request->method = line;
    request->path = target;
    request->query = query;
    return 0;
}

static bool is_token (const char * value) {
    if (*value == '\0') {
        return false;
    }
    for (; *value; value++) {
        if (!isalnum((unsigned char)*value)
            && strchr("!#$%&'*+-.^_" "\x60" "|~", *value) == NULL) {
            return false;
        }
    }
    return true;
}

static int parse_field (char * line, fields_t * fields, request_t * request) {
    char * value = strchr(line, ':');
    if (value == NULL) {
        return 400;
    }
    *value++ = '\0';
    if (!is_token(line)) {
        return 400;
    }
    value += strspn(value, " \t");
    char * end = value + strlen(value);
    while (end > value && (end[-1] == ' ' || end[-1] == '\t')) {
        *--end = '\0';
    }
    for (const unsigned char * byte = (void *)value; *byte; byte++) {
        if ((*byte < 32 && *byte != '\t') || *byte == 127) {
            return 400;
        }
    }
    if (strcasecmp(line, "Host") == 0) {
        if (fields->has_host || !is_valid_host(value)) {
            return 400;
        }
        fields->has_host = true;
    } else if (strcasecmp(line, "Content-Type") == 0) {
        request->content_type = value;
    } else if (strcasecmp(line, "Content-Length") == 0) {
        /* 값 문자열을 body_length에 숫자로 저장해야 본문 수신을 끝낼 때를 안다. */
        return parse_length(value, fields, request);
    } else if (strcasecmp(line, "Transfer-Encoding") == 0) {
        fields->has_encoding = true;
    }
    return 0;
}

static bool is_valid_host (char * host) {
    if (*host == '[') {
        char * end = strchr(host, ']');
        struct in6_addr address;
        if (end == NULL) {
            return false;
        }
        *end = '\0';
        return inet_pton(AF_INET6, host + 1, &address) == 1
            && (end[1] == '\0' || (end[1] == ':' && is_valid_port(end + 2)));
    }
    size_t length = strspn(host,
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789.-_");
    if (length == 0) {
        return false;
    }
    return host[length] == '\0'
        || (host[length] == ':' && is_valid_port(host + length + 1));
}

static bool is_valid_port (const char * port) {
    return *port != '\0' && port[strspn(port, "0123456789")] == '\0'
        && strtoul(port, NULL, 10) <= 65535;
}

static int parse_length (const char * value, fields_t * fields,
                         request_t * request) {
    if (fields->has_length || *value == '\0') {
        return 400;
    }
    fields->has_length = true;
    for (; *value; value++) {
        if (*value < '0' || *value > '9') {
            return 400;
        }
        if (!fields->length_exceeded) {
            request->body_length = request->body_length * 10 + (*value - '0');
            fields->length_exceeded = request->body_length > BODY_LIMIT;
        }
    }
    return 0;
}
