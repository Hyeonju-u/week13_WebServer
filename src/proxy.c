#include "proxy.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define REPLY_LIMIT (HEADER_LIMIT + BODY_LIMIT)

static int send_backend_request(proxy_t * proxy, const request_t * request,
                                const char * body);
static int read_backend_response(proxy_t * proxy, response_t * response);
static int parse_reply_headers(proxy_t * proxy);
static int parse_reply_field(proxy_t * proxy, char * line);

/* ── 공개 함수 ── */

bool set_proxy_route (proxy_route_t * route, const char * prefix,
                      const char * host, const char * port) {
    size_t length = strlen(prefix);
    if (length < 2 || prefix[0] != '/' || prefix[length - 1] == '/'
        || strpbrk(prefix, "?# \t\r\n") != NULL
        || *port == '\0' || port[strspn(port, "0123456789")] != '\0') {
        return false;
    }
    unsigned long number = strtoul(port, NULL, 10);
    if (!number || number > 65535
        || inet_pton(AF_INET, host, &route->address.sin_addr) != 1) {
        return false;
    }
    route->address.sin_family = AF_INET;
    route->address.sin_port = htons((unsigned short)number);
    route->host = host;
    route->prefix = prefix;
    return true;
}

/* 경로 구성요소 단위로 접두사를 제거한다. 일치하지 않으면 NULL이다. */
const char * find_backend_path (const proxy_route_t * route,
                                const char * path) {
    if (route->prefix == NULL) {
        return NULL;
    }
    size_t length = strlen(route->prefix);
    if (strncmp(path, route->prefix, length) != 0) {
        return NULL;
    }
    if (path[length] == '\0') {
        return "/";
    }
    return path[length] == '/' ? path + length : NULL;
}

int64_t monotonic_ms (void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

/* 연결 진행 중에도 반환한다. 호출자는 destroy_backend_connection()로 해제한다. */
proxy_t * create_backend_connection (const proxy_route_t * route,
                                     const char * path,
                                     const request_t * request) {
    proxy_t * proxy = calloc(1, sizeof(*proxy));
    if (proxy == NULL) {
        return NULL;
    }
    /* 연결부터 응답 완료까지 같은 마감 시각을 사용한다. */
    proxy->deadline = monotonic_ms() + PROXY_TIMEOUT_MS;
    proxy->expected = SIZE_MAX;
    proxy->socket_fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    int length = snprintf(proxy->outgoing, sizeof(proxy->outgoing),
        "%s %s%s%s HTTP/1.1\r\nHost: %s:%u\r\nContent-Length: %zu\r\n"
        "%s%s%sConnection: close\r\n\r\n", request->method, path,
        request->query ? "?" : "", request->query ? request->query : "",
        route->host, (unsigned)ntohs(route->address.sin_port),
        request->body_length, request->content_type ? "Content-Type: " : "",
        request->content_type ? request->content_type : "",
        request->content_type ? "\r\n" : "");
    if (proxy->socket_fd < 0 || length < 0
        || (size_t)length >= sizeof(proxy->outgoing)) {
        destroy_backend_connection(proxy);
        return NULL;
    }
    proxy->outgoing_length = (size_t)length;
    if (connect(proxy->socket_fd, (const struct sockaddr *)&route->address,
                sizeof(route->address)) < 0 && errno != EINPROGRESS) {
        destroy_backend_connection(proxy);
        return NULL;
    }
    return proxy;
}

/* 0은 다음 이벤트 대기, 1은 응답 준비 완료, 그 밖의 값은 오류 상태다. */
int send_request_or_read_response (proxy_t * proxy, const request_t * request,
                                   const char * body, response_t * response) {
    if (monotonic_ms() >= proxy->deadline) {
        return 504;
    }
    if (!proxy->reading) {
        return send_backend_request(proxy, request, body);
    }
    return read_backend_response(proxy, response);
}

void destroy_backend_connection (proxy_t * proxy) {
    if (proxy == NULL) {
        return;
    }
    if (proxy->socket_fd >= 0) {
        close(proxy->socket_fd);
    }
    free(proxy->received);
    free(proxy);
}

/* ── 도우미 함수 ── */

static int send_backend_request (proxy_t * proxy, const request_t * request,
                                 const char * body) {
    int error;
    socklen_t size = sizeof(error);
    /* 연결 실패 때도 소켓이 깨어날 수 있어 SO_ERROR로 성공 여부를 확인한다. */
    if (getsockopt(proxy->socket_fd, SOL_SOCKET, SO_ERROR, &error, &size) < 0
        || error != 0) {
        return 502;
    }
    const char * data;
    size_t length;
    if (proxy->sent < proxy->outgoing_length) {
        data = proxy->outgoing + proxy->sent;
        length = proxy->outgoing_length - proxy->sent;
    } else {
        size_t offset = proxy->sent - proxy->outgoing_length;
        data = body + offset;
        length = request->body_length - offset;
    }
    ssize_t count = send(proxy->socket_fd, data, length, 0);
    if (count < 0) {
        return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR
            ? 0 : 502;
    }
    proxy->sent += (size_t)count;
    proxy->reading = proxy->sent == proxy->outgoing_length
        + request->body_length;
    return 0;
}

static int read_backend_response (proxy_t * proxy, response_t * response) {
    char chunk[8192];
    ssize_t count = recv(proxy->socket_fd, chunk, sizeof(chunk), 0);
    if (count < 0) {
        return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR
            ? 0 : 502;
    }
    size_t previous = proxy->used;
    size_t needed = previous + (size_t)count;
    if (needed > REPLY_LIMIT) {
        return 502;
    }
    if (needed > proxy->capacity) {
        size_t capacity = needed * 2;
        if (capacity > REPLY_LIMIT) {
            capacity = REPLY_LIMIT;
        }
        char * resized = realloc(proxy->received, capacity);
        if (resized == NULL) {
            return 500;
        }
        proxy->received = resized;
        proxy->capacity = capacity;
    }
    if (count > 0) {
        memcpy(proxy->received + previous, chunk, (size_t)count);
        proxy->used = needed;
    }
    if (proxy->header_end == 0) {
        proxy->header_end = find_header_end(proxy->received, needed,
                                            previous > 3 ? previous - 3 : 0);
        if (proxy->header_end == 0) {
            return count == 0 || needed >= HEADER_LIMIT ? 502 : 0;
        }
        if (proxy->header_end > HEADER_LIMIT || parse_reply_headers(proxy)) {
            return 502;
        }
    }
    size_t length = needed - proxy->header_end;
    if (length > BODY_LIMIT || length > proxy->expected) {
        return 502;
    }
    /* 길이를 모르면 expected는 SIZE_MAX다. 이때는 recv()가 0이면 본문이 끝난다. */
    if (length != proxy->expected && count > 0) {
        return 0;
    }
    if (proxy->expected != SIZE_MAX && length != proxy->expected) {
        return 502;
    }
    prepare_proxy_response(response, proxy->status, proxy->type,
                           proxy->received + proxy->header_end, length);
    return 1;
}

static int parse_reply_headers (proxy_t * proxy) {
    char * line = proxy->received;
    if (memchr(line, '\0', proxy->header_end) != NULL) {
        return 502;
    }
    line[proxy->header_end - 2] = '\0';
    char * end = strstr(line, "\r\n");
    if (end == NULL) {
        return 502;
    }
    *end = '\0';
    if (strncmp(line, "HTTP/1.1 ", 9) && strncmp(line, "HTTP/1.0 ", 9)) {
        return 502;
    }
    const char * status = line + 9;
    size_t length = strlen(status);
    if (length < 3 || length >= sizeof(proxy->status)
        || strspn(status, "0123456789") != 3 || status[0] < '2'
        || status[0] > '5' || (length > 3 && status[3] != ' ')) {
        return 502;
    }
    for (size_t i = 3; i < length; i++) {
        if ((unsigned char)status[i] < 32 || status[i] == 127) {
            return 502;
        }
    }
    strcpy(proxy->status, status);
    strcpy(proxy->type, "application/octet-stream");
    line = end + 2;
    while (*line) {
        end = strstr(line, "\r\n");
        if (end == NULL) {
            return 502;
        }
        *end = '\0';
        if (parse_reply_field(proxy, line) != 0) {
            return 502;
        }
        line = end + 2;
    }
    if (!strncmp(status, "204", 3) || !strncmp(status, "304", 3)) {
        proxy->expected = 0;
    }
    return 0;
}

static int parse_reply_field (proxy_t * proxy, char * line) {
    char * value = strchr(line, ':');
    if (value == NULL || value == line) {
        return 502;
    }
    *value++ = '\0';
    for (const char * name = line; *name; name++) {
        if (!isalnum((unsigned char)*name)
            && strchr("!#$%&'*+-.^_\x60|~", *name) == NULL) {
            return 502;
        }
    }
    value += strspn(value, " \t");
    char * end = value + strlen(value);
    while (end > value && (end[-1] == ' ' || end[-1] == '\t')) {
        *--end = '\0';
    }
    for (const unsigned char * byte = (void *)value; *byte; byte++) {
        if ((*byte < 32 && *byte != '\t') || *byte == 127) {
            return 502;
        }
    }
    if (strcasecmp(line, "Content-Length") == 0) {
        if (proxy->expected != SIZE_MAX || *value == '\0'
            || value[strspn(value, "0123456789")] != '\0') {
            return 502;
        }
        unsigned long length = strtoul(value, NULL, 10);
        if (length > BODY_LIMIT) {
            return 502;
        }
        proxy->expected = length;
    } else if (strcasecmp(line, "Content-Type") == 0) {
        if (strlen(value) >= sizeof(proxy->type)) {
            return 502;
        }
        strcpy(proxy->type, value);
    } else if (strcasecmp(line, "Transfer-Encoding") == 0) {
        return 502;
    }
    return 0;
}
