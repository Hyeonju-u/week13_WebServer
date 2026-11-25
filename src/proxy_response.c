#include "proxy_response.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "http_request.h"
#include "proxy_request.h"

/* (G35) "HTTP/1.x NNN" = 12글자. 상태 숫자는 9번째 글자부터 */
#define STATUS_LINE_MIN_LENGTH 12
#define STATUS_CODE_START 9
#define MIN_STATUS 100
#define MAX_STATUS 599
/* (G8) 101 Switching Protocols는 Upgrade를 지웠으니 받지 않는다 (G35) */
#define SWITCHING_PROTOCOLS 101
#define NO_CONTENT 204
#define NOT_MODIFIED 304

/* (G9) Content-Length 한 줄을 훑은 결과 */
typedef enum {
    LENGTH_OK,
    LENGTH_BAD,
    LENGTH_TOO_LARGE
} length_result;

static size_t find_status_line_end (const char * data, size_t head_length);
static bool parse_status_line (const char * data, size_t line_end,
                               int * status);
static bool is_digit (char letter);
static void set_headers_area (size_t line_end, backend_head_t * head);
static backend_head_result parse_backend_headers (const char * data,
                                                  size_t max_body,
                                                  backend_head_t * head);
static bool is_name (const header_view_t * line, const char * name);
static length_result read_content_length (const header_view_t * line,
                                          size_t max_body, size_t * length);
static size_t append_bytes (char * buffer, size_t length,
                            const char * bytes, size_t count);
static bool is_passed_header (const char * headers, size_t headers_length,
                              const header_view_t * line);

/* ── 공개 함수 ── */

/* (G8, G12, G27, G35) 헤더 끝 → 상태 줄 → (1xx면 여기서) → 헤더 줄 */
backend_head_result parse_backend_head (const char * data, size_t length,
                                        size_t max_body,
                                        backend_head_t * head) {
    size_t head_length = 0;
    int find_status = 0;
    int status = 0;

    find_result found = find_header_end(data, length, &head_length,
                                        &find_status);
    if (found == FIND_NEED_MORE) {
        return BACKEND_HEAD_NEED_MORE;
    }
    if (found == FIND_BAD) {
        return BACKEND_HEAD_BAD;
    }
    size_t line_end = find_status_line_end(data, head_length);
    if (!parse_status_line(data, line_end, &status)
        || status == SWITCHING_PROTOCOLS) {
        return BACKEND_HEAD_BAD;
    }
    memset(head, 0, sizeof(*head));
    head->status = status;
    head->head_length = head_length;
    head->status_line_end = line_end;
    if (status < 200) {
        return BACKEND_HEAD_INTERIM;
    }
    set_headers_area(line_end, head);
    head->has_body = status != NO_CONTENT && status != NOT_MODIFIED;
    return parse_backend_headers(data, max_body, head);
}

/* (G10, G35, G44) 붙이는 줄은 Content-Length 최대 38 + Connection 24 바이트라
 * MAX_RESPONSE_HEAD_SIZE 여유(512) 안에 든다. 넘침 처리는 두지 않는다 (S27) */
size_t write_client_head (const char * data, const backend_head_t * head,
                          size_t body_length, bool keep_alive,
                          char * buffer) {
    const char * headers = data + head->headers_start;
    char line_text[64];
    size_t position = 0;
    header_view_t line;

    size_t length = append_bytes(buffer, 0, "HTTP/1.1 ", 9);
    length = append_bytes(buffer, length, data + STATUS_CODE_START,
                          head->status_line_end - STATUS_CODE_START);
    length = append_bytes(buffer, length, "\r\n", 2);
    while (next_header_line(headers, head->headers_length, &position,
                            &line) == LINE_OK) {
        if (!is_passed_header(headers, head->headers_length, &line)) {
            continue;
        }
        length = append_bytes(buffer, length, line.name, line.name_length);
        length = append_bytes(buffer, length, ": ", 2);
        length = append_bytes(buffer, length, line.value, line.value_length);
        length = append_bytes(buffer, length, "\r\n", 2);
    }
    if (head->has_body) {
        int count = snprintf(line_text, sizeof(line_text),
                             "Content-Length: %zu\r\n", body_length);
        length = append_bytes(buffer, length, line_text, (size_t) count);
    }
    const char * connection = keep_alive ? "Connection: keep-alive\r\n\r\n"
                                         : "Connection: close\r\n\r\n";
    return append_bytes(buffer, length, connection, strlen(connection));
}

/* ── 도우미 함수 ── */

/* (G8) find_header_end가 찾은 헤더는 \r\n\r\n으로 끝나서 \r\n이 꼭 있다 */
static size_t find_status_line_end (const char * data, size_t head_length) {
    for (size_t i = 0; i + 1 < head_length; i++) {
        if (data[i] == '\r' && data[i + 1] == '\n') {
            return i;
        }
    }
    return head_length;
}

/* (G12, G35) "HTTP/1.<숫자> <숫자 3개>" 다음은 줄 끝이거나 공백 + 사유 */
static bool parse_status_line (const char * data, size_t line_end,
                               int * status) {
    if (line_end < STATUS_LINE_MIN_LENGTH
        || memcmp(data, "HTTP/1.", 7) != 0 || !is_digit(data[7])
        || data[8] != ' ') {
        return false;
    }
    for (size_t i = STATUS_CODE_START; i < STATUS_LINE_MIN_LENGTH; i++) {
        if (!is_digit(data[i])) {
            return false;
        }
    }
    if (line_end > STATUS_LINE_MIN_LENGTH
        && data[STATUS_LINE_MIN_LENGTH] != ' ') {
        return false;
    }
    *status = (data[9] - '0') * 100 + (data[10] - '0') * 10
              + (data[11] - '0');
    return *status >= MIN_STATUS && *status <= MAX_STATUS;
}

static bool is_digit (char letter) {
    return letter >= '0' && letter <= '9';
}

/* (P22) 상태 줄 다음부터 마지막 빈 줄 앞까지. 헤더 줄이 없으면 길이 0 */
static void set_headers_area (size_t line_end, backend_head_t * head) {
    size_t start = line_end + 2;
    size_t end = head->head_length - 2;

    head->headers_start = start;
    head->headers_length = end > start ? end - start : 0;
}

/* (G8, G9, G12) 형식 깨짐 → chunked → 상한 초과 순서로 먼저 걸린 것.
 * Content-Length가 두 번이면 깨진 응답 (B30과 같은 이유) */
static backend_head_result parse_backend_headers (const char * data,
                                                  size_t max_body,
                                                  backend_head_t * head) {
    const char * headers = data + head->headers_start;
    size_t position = 0;
    header_view_t line;
    line_result result;
    bool is_chunked = false;
    length_result length = LENGTH_OK;

    while ((result = next_header_line(headers, head->headers_length,
                                      &position, &line)) == LINE_OK) {
        if (is_name(&line, "Transfer-Encoding")) {
            is_chunked = true;
        }
        if (!is_name(&line, "Content-Length")) {
            continue;
        }
        if (head->has_content_length) {
            return BACKEND_HEAD_BAD;
        }
        head->has_content_length = true;
        length = read_content_length(&line, max_body, &head->content_length);
    }
    if (result == LINE_BAD || length == LENGTH_BAD) {
        return BACKEND_HEAD_BAD;
    }
    if (is_chunked) {
        return BACKEND_HEAD_CHUNKED;
    }
    if (length == LENGTH_TOO_LARGE && head->has_body) {
        return BACKEND_HEAD_TOO_LARGE;
    }
    return BACKEND_HEAD_OK;
}

/* (P8) */
static bool is_name (const header_view_t * line, const char * name) {
    return line->name_length == strlen(name)
           && strncasecmp(line->name, name, line->name_length) == 0;
}

/* (G9, P9) 상한을 넘는 순간 멈춰서 넘침으로 작은 값이 되지 않게 한다 */
static length_result read_content_length (const header_view_t * line,
                                          size_t max_body, size_t * length) {
    size_t total = 0;

    if (line->value_length == 0) {
        return LENGTH_BAD;
    }
    for (size_t i = 0; i < line->value_length; i++) {
        if (!is_digit(line->value[i])) {
            return LENGTH_BAD;
        }
        total = total * 10 + (size_t) (line->value[i] - '0');
        if (total > max_body) {
            return LENGTH_TOO_LARGE;
        }
    }
    *length = total;
    return LENGTH_OK;
}

/* (G10) 반환: 이어 쓴 뒤의 길이 */
static size_t append_bytes (char * buffer, size_t length,
                            const char * bytes, size_t count) {
    memcpy(buffer + length, bytes, count);
    return length + count;
}

/* (G10) hop-by-hop은 지우고, Content-Length는 받은 길이로 다시 쓴다 */
static bool is_passed_header (const char * headers, size_t headers_length,
                              const header_view_t * line) {
    if (is_name(line, "Content-Length")) {
        return false;
    }
    return !is_hop_by_hop_header(headers, headers_length, line);
}
