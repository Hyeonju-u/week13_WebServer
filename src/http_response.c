#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "http_response.h"

/* (S13) 200이 아닌 응답의 본문 형식 */
#define ERROR_CONTENT_TYPE "text/html; charset=utf-8"
#define ERROR_BODY_FORMAT \
    "<!DOCTYPE html><html><head><title>%d %s</title></head>" \
    "<body><h1>%d %s</h1></body></html>\n"

/* (S13) 가장 긴 사유 문구(431)를 넣어도 남는 크기 */
#define MAX_ERROR_BODY_SIZE 256

/* (S12) IMF-fixdate 29글자 + '\0' */
#define HTTP_DATE_SIZE 32

typedef struct {
    int status;
    const char * reason;
} status_reason_t;

/* (R1g, S12, L34, G38) 5번에 적힌 코드와 사유 문구 + 4단계 502·504 */
static const status_reason_t status_reasons[] = {
    { 200, "OK" },
    { 301, "Moved Permanently" },
    { 400, "Bad Request" },
    { 403, "Forbidden" },
    { 404, "Not Found" },
    { 405, "Method Not Allowed" },
    { 408, "Request Timeout" },
    { 411, "Length Required" },
    { 413, "Content Too Large" },
    { 414, "URI Too Long" },
    { 431, "Request Header Fields Too Large" },
    { 500, "Internal Server Error" },
    { 501, "Not Implemented" },
    { 502, "Bad Gateway" },
    { 504, "Gateway Timeout" },
    { 505, "HTTP Version Not Supported" },
};

static int pick_known_status (int status);
static const char * find_reason (int status);
static size_t write_error_body (int status, char * body);
static size_t append_text (char * buffer, size_t length,
                           const char * format, ...);
static size_t append_date (char * buffer, size_t length, int status,
                           time_t now);
static bool write_http_date (time_t now, char * date);
static size_t append_status_headers (char * buffer, size_t length,
                                     const response_head_t * head,
                                     int status);
static size_t append_bytes (char * buffer, size_t length,
                            const char * bytes, size_t count);

/* ── 공개 함수 ── */

/* (R1f, R1g, S12, S13, S19, S23, S27, S28) */
size_t write_response_head (const response_head_t * head, time_t now,
                            char * buffer) {
    int status = pick_known_status(head->status);
    const char * content_type = head->content_type;
    size_t content_length = head->content_length;
    char body[MAX_ERROR_BODY_SIZE];
    size_t body_length = 0;

    if (status != 200) {
        body_length = write_error_body(status, body);
        content_type = ERROR_CONTENT_TYPE;
        content_length = body_length;
    }
    size_t length = append_text(buffer, 0, "HTTP/1.1 %d %s\r\n",
                                status, find_reason(status));
    length = append_text(buffer, length, "Content-Type: %s\r\n",
                         content_type);
    length = append_text(buffer, length, "Content-Length: %zu\r\n",
                         content_length);
    length = append_text(buffer, length, "Connection: %s\r\n",
                         head->keep_alive ? "keep-alive" : "close");
    length = append_date(buffer, length, status, now);
    length = append_status_headers(buffer, length, head, status);
    length = append_text(buffer, length, "\r\n");
    return append_bytes(buffer, length, body, body_length);
}

/* ── 도우미 함수 ── */

/* (R1g) 표에 없는 코드는 500으로 답한다. 사유 문구 없는 상태 줄을 내보내지 않기 위해 */
static int pick_known_status (int status) {
    if (find_reason(status) == NULL) {
        return 500;
    }
    return status;
}

/* 반환: 사유 문구, NULL = 표에 없는 코드 */
static const char * find_reason (int status) {
    size_t count = sizeof(status_reasons) / sizeof(status_reasons[0]);

    for (size_t i = 0; i < count; i++) {
        if (status_reasons[i].status == status) {
            return status_reasons[i].reason;
        }
    }
    return NULL;
}

/* (S13, S19) 코드와 사유 문구만 넣는다. 손님이 보낸 경로는 넣지 않는다.
 * 반환: 본문 바이트 수 */
static size_t write_error_body (int status, char * body) {
    const char * reason = find_reason(status);
    int written = snprintf(body, MAX_ERROR_BODY_SIZE, ERROR_BODY_FORMAT,
                           status, reason, status, reason);
    if (written < 0) {
        return 0;
    }
    return (size_t) written;
}

/* (S27) buffer는 MAX_RESPONSE_HEAD_SIZE. 경로가 1단계에서 막혀 들어오므로
 * 넘치지 않는다. 반환: 새 길이 */
static size_t append_text (char * buffer, size_t length,
                           const char * format, ...) {
    va_list arguments;

    va_start(arguments, format);
    int written = vsnprintf(buffer + length, MAX_RESPONSE_HEAD_SIZE - length,
                            format, arguments);
    va_end(arguments);
    if (written < 0) {
        return length;
    }
    return length + (size_t) written;
}

/* (S12, S23) 5xx에는 넣지 않는다 */
static size_t append_date (char * buffer, size_t length, int status,
                           time_t now) {
    char date[HTTP_DATE_SIZE];

    if (status >= 500) {
        return length;
    }
    if (!write_http_date(now, date)) {
        return length;
    }
    return append_text(buffer, length, "Date: %s\r\n", date);
}

/* (S12) IMF-fixdate. setlocale을 부르지 않으므로 요일·달 이름이 영어다.
 * 반환: false = 시각을 날짜로 바꾸지 못함 */
static bool write_http_date (time_t now, char * date) {
    struct tm parts;

    if (gmtime_r(&now, &parts) == NULL) {
        return false;
    }
    return strftime(date, HTTP_DATE_SIZE, "%a, %d %b %Y %H:%M:%S GMT",
                    &parts) > 0;
}

/* (S8) 405에 Allow, (S9, S22) 301에 Location = 원문 경로 + '/' */
static size_t append_status_headers (char * buffer, size_t length,
                                     const response_head_t * head,
                                     int status) {
    if (status == 405) {
        return append_text(buffer, length, "Allow: GET\r\n");
    }
    if (status == 301) {
        return append_text(buffer, length, "Location: %.*s/\r\n",
                           (int) head->location_length, head->location);
    }
    return length;
}

/* (S27) 오류 본문을 헤더 바로 뒤에 이어 쓴다 */
static size_t append_bytes (char * buffer, size_t length,
                            const char * bytes, size_t count) {
    memcpy(buffer + length, bytes, count);
    return length + count;
}
