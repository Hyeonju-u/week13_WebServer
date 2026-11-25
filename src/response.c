#include "response.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct {
    const char * extension;
    const char * type;
} mime_type_t;

static void build_response_headers(response_t * response, const char * status,
                                   const char * type, uintmax_t length);
static int decode_path(const char * path, char * decoded);
static int parse_hex(char digit);
static int open_static_file(int root, const char * path);
static const char * find_file_content_type(const char * path);
static const char * find_status(int status);
static ssize_t send_bytes(int client, const char * data, size_t length);

/* ── 공개 함수 ── */

/* 응답 전송이 끝날 때까지 요청 본문을 빌린다. 열린 파일은 연결이 소유한다. */
void build_local_response (response_t * response, const request_t * request,
                           const char * body, int root) {
    if (strcmp(request->method, "POST") == 0) {
        if (strcmp(request->path, "/api/echo") != 0) {
            prepare_error(response, 404);
            return;
        }
        /* 에코는 받은 본문을 그대로 보내므로 같은 버퍼를 사용한다. */
        response->body = body;
        response->pending = request->body_length;
        build_response_headers(response, "200 OK", "application/json",
                               request->body_length);
        return;
    }
    if (strcmp(request->method, "GET") != 0) {
        prepare_error(response, 405);
        return;
    }
    char path[TARGET_LIMIT + 1];
    /* 26번 요청 경로에 퍼센트 인코딩이 있으면 원래 바이트로 되돌린다. */
    int status = decode_path(request->path, path);
    if (status != 0) {
        prepare_error(response, status);
        return;
    }
    const char * file_path = strcmp(path, "/") == 0 ? "/index.html" : path;
    /* 27번 public 디렉터리를 기준으로 sample.html을 찾는다. */
    int file = open_static_file(root, file_path);
    struct stat info;
    if (file < 0) {
        prepare_error(response, -file);
        return;
    }
    response->file = file;
    /* 29번 파일 크기(info.st_size)를 구해 응답의 Content-Length로 쓴다. */
    if (fstat(file, &info) < 0 || !S_ISREG(info.st_mode)) {
        prepare_error(response, 404);
        return;
    }
    /* 30번 find_file_content_type()이 .html에 맞는 Content-Type을 반환한다.
     * 31번 build_response_headers()가 이 값과 파일 크기로 200 응답 헤더를 만든다. */
    build_response_headers(response, "200 OK",
                           find_file_content_type(file_path), info.st_size);
}

void prepare_error (response_t * response, int status) {
    response->body = find_status(status);
    response->pending = strlen(response->body);
    build_response_headers(response, response->body, "text/plain",
                           response->pending);
}

/* 전송이 끝날 때까지 프록시가 소유한 본문을 빌린다. */
void prepare_proxy_response (response_t * response, const char * status,
                             const char * type, const char * body,
                             size_t size) {
    if (!strncmp(status, "204", 3) || !strncmp(status, "304", 3)) {
        response->header_length = (size_t)snprintf(response->header,
            sizeof(response->header),
            "HTTP/1.1 %s\r\nConnection: close\r\n\r\n", status);
        return;
    }
    response->body = body;
    response->pending = size;
    build_response_headers(response, status, type, size);
}

/* 0이면 다음 쓰기 이벤트를 기다린다. 1은 완료, -1은 연결 종료다. */
int send_response (response_t * response, int client) {
    if (response->header_sent < response->header_length) {
        /* 34번 send()로 헤더를 먼저 보내고 보낸 길이를 header_sent에 남긴다. */
        ssize_t count = send_bytes(client,
            response->header + response->header_sent,
            response->header_length - response->header_sent);
        if (count < 0) {
            return -1;
        }
        /* 다음 호출은 헤더 시작 주소에 header_sent를 더한 위치에서 재개한다. */
        response->header_sent += (size_t)count;
        return response->header_sent == response->header_length
            && response->remaining == 0;
    }
    if (response->pending == 0) {
        size_t length = sizeof(response->chunk);
        if (response->remaining < length) {
            length = (size_t)response->remaining;
        }
        /* 35번 다음 쓰기 이벤트에서 파일 본문을 버퍼 크기만큼 읽는다. */
        ssize_t count = read(response->file, response->chunk, length);
        if (count < 0 && errno == EINTR) {
            return 0;
        }
        if (count <= 0) {
            return -1;
        }
        response->pending = (size_t)count;
        response->body = response->chunk;
    }
    /* 36번 send()로 본문을 보내고 남은 양만큼 다음 이벤트에서 이어 보낸다. */
    ssize_t count = send_bytes(client, response->body, response->pending);
    if (count < 0) {
        return -1;
    }
    /* 실제 보낸 만큼만 이동한다. pending은 현재 버퍼, remaining은 본문 전체의 잔량이다. */
    response->body += count;
    response->pending -= (size_t)count;
    response->remaining -= (size_t)count;
    return response->remaining == 0;
}

/* ── 도우미 함수 ── */

static void build_response_headers (response_t * response, const char * status,
                                    const char * type, uintmax_t length) {
    const char * allow = strncmp(status, "405", 3) == 0
        ? "Allow: GET, POST\r\n" : "";
    response->header_length = (size_t)snprintf(response->header,
        sizeof(response->header), "HTTP/1.1 %s\r\nContent-Type: %s\r\n"
        "Content-Length: %" PRIuMAX "\r\n%sConnection: close\r\n\r\n",
        status, type, length, allow);
    response->remaining = length;
}

static int decode_path (const char * path, char * decoded) {
    while (*path) {
        unsigned char byte = (unsigned char)*path++;
        if (byte == '%') {
            int high = parse_hex(*path);
            int low = high < 0 ? -1 : parse_hex(path[1]);
            if (low < 0) {
                return 400;
            }
            byte = (unsigned char)(high * 16 + low);
            path += 2;
        }
        if (byte < 32 || byte == 127 || byte == '\\') {
            return 400;
        }
        *decoded++ = (char)byte;
    }
    *decoded = '\0';
    return 0;
}

static int parse_hex (char digit) {
    const char * digits = "0123456789abcdef";
    if (digit >= 'A' && digit <= 'F') {
        digit = (char)(digit - 'A' + 'a');
    }
    const char * found = strchr(digits, digit);
    return digit == '\0' || found == NULL ? -1 : (int)(found - digits);
}

/* 루트 fd에서 한 단계씩 열어 심볼릭 링크 교체로 경계가 바뀌지 않게 한다. */
static int open_static_file (int root, const char * path) {
    char components[TARGET_LIMIT + 1];
    strcpy(components, path + 1);
    int directory = dup(root);
    if (directory < 0) {
        return -500;
    }
    char * part = components;
    for (;;) {
        char * separator = strchr(part, '/');
        int flags = O_RDONLY | O_NOFOLLOW | O_NONBLOCK;
        if (separator != NULL) {
            *separator = '\0';
            flags |= O_DIRECTORY;
        }
        if (strcmp(part, "..") == 0) {
            close(directory);
            return -403;
        }
        /* 28번 현재 디렉터리 fd를 기준으로 실제 파일이나 하위 디렉터리를 연다. */
        int next = openat(directory, part, flags);
        int error = errno;
        close(directory);
        if (next < 0) {
            if (error == EACCES || error == ELOOP) {
                return -403;
            }
            return error == ENOENT || error == ENOTDIR ? -404 : -500;
        }
        if (separator == NULL) {
            return next;
        }
        directory = next;
        part = separator + 1;
    }
}

static const char * find_file_content_type (const char * path) {
    const char * extension = strrchr(path, '.');
    static const mime_type_t types[] = {
        {".html", "text/html; charset=utf-8"}, {".css", "text/css"},
        {".js", "text/javascript"}, {".json", "application/json"},
        {".txt", "text/plain"}, {".png", "image/png"}, {".jpg", "image/jpeg"},
        {".svg", "image/svg+xml"}
    };
    for (size_t i = 0; extension != NULL && i < sizeof(types)/sizeof(*types);
         i++) {
        if (strcasecmp(extension, types[i].extension) == 0) {
            return types[i].type;
        }
    }
    return "application/octet-stream";
}

static const char * find_status (int status) {
    switch (status) {
        case 400: return "400 Bad Request";
        case 403: return "403 Forbidden";
        case 404: return "404 Not Found";
        case 405: return "405 Method Not Allowed";
        case 411: return "411 Length Required";
        case 413: return "413 Content Too Large";
        case 414: return "414 URI Too Long";
        case 431: return "431 Request Header Fields Too Large";
        case 501: return "501 Not Implemented";
        case 502: return "502 Bad Gateway";
        case 504: return "504 Gateway Timeout";
        default: return "500 Internal Server Error";
    }
}

static ssize_t send_bytes (int client, const char * data, size_t length) {
    ssize_t count = send(client, data, length, 0);
    if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK
                      || errno == EINTR)) {
        return 0;
    }
    return count;
}
