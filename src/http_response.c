#include "http_response.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#define ERROR_BODY_MAX 256

typedef struct {
    int status_code;
    const char * text;
} status_entry_t;

/* 상태 코드와 사유 문구는 이 표 한 곳에서만 관리한다 (RES-03) */
static const status_entry_t status_table[] = {
    { 100, "Continue" },
    { 200, "OK" },
    { 301, "Moved Permanently" },
    { 400, "Bad Request" },
    { 403, "Forbidden" },
    { 404, "Not Found" },
    { 405, "Method Not Allowed" },
    { 411, "Length Required" },
    { 413, "Payload Too Large" },
    { 414, "URI Too Long" },
    { 417, "Expectation Failed" },
    { 431, "Request Header Fields Too Large" },
    { 500, "Internal Server Error" },
    { 501, "Not Implemented" },
    { 502, "Bad Gateway" },
    { 503, "Service Unavailable" },
    { 504, "Gateway Timeout" },
    { 505, "HTTP Version Not Supported" },
};

static int append_date_header (byte_buffer_t * output);

/* ── 공개 함수 ── */

const char * get_status_text (int status_code) {
    size_t count = sizeof(status_table) / sizeof(status_table[0]);
    for (size_t i = 0; i < count; i++) {
        if (status_table[i].status_code == status_code) {
            return status_table[i].text;
        }
    }
    return "Unknown";
}

/* 실패하면 output에 일부가 남을 수 있으니 호출자가 버퍼를 비워야 한다 */
int append_response_header (byte_buffer_t * output, int status_code,
    const char * content_type, size_t content_length, int keep_alive,
    const char * extra_headers) {
    if (append_format(output, "HTTP/1.1 %d %s\r\n", status_code,
            get_status_text(status_code)) != 0) {
        return -1;
    }
    if (append_format(output, "Content-Type: %s\r\n"
            "Content-Length: %zu\r\nConnection: %s\r\n", content_type,
            content_length, keep_alive ? "keep-alive" : "close") != 0) {
        return -1;
    }
    if (append_date_header(output) != 0) {
        return -1;
    }
    if (extra_headers != NULL
        && append_bytes(output, extra_headers, strlen(extra_headers)) != 0) {
        return -1;
    }
    return append_bytes(output, "\r\n", 2);
}

/* HEAD에도 GET과 같은 Content-Length를 보내고 본문만 뺀다 (RES-10) */
int append_error_response (byte_buffer_t * output, int status_code,
    int keep_alive, int is_head, const char * extra_headers) {
    char body[ERROR_BODY_MAX];
    const char * text = get_status_text(status_code);
    int body_length = snprintf(body, sizeof(body),
        "<html><head><title>%d %s</title></head>"
        "<body><h1>%d %s</h1></body></html>\n",
        status_code, text, status_code, text);
    if (body_length < 0 || (size_t) body_length >= sizeof(body)) {
        return -1;
    }
    if (append_response_header(output, status_code, "text/html; charset=utf-8",
            (size_t) body_length, keep_alive, extra_headers) != 0) {
        return -1;
    }
    if (is_head) {
        return 0;
    }
    return append_bytes(output, body, (size_t) body_length);
}

/* 1xx 응답은 상태 라인과 빈 줄만 보낸다 (RES-04) */
int append_continue_response (byte_buffer_t * output) {
    return append_bytes(output, "HTTP/1.1 100 Continue\r\n\r\n", 25);
}

/* ── 도우미 함수 ── */

static int append_date_header (byte_buffer_t * output) {
    time_t now = time(NULL);
    struct tm parts;
    if (gmtime_r(&now, &parts) == NULL) {
        return -1;
    }
    char text[64];
    if (strftime(text, sizeof(text), "%a, %d %b %Y %H:%M:%S GMT",
            &parts) == 0) {
        return -1;
    }
    return append_format(output, "Date: %s\r\n", text);
}
