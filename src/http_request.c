#include "http_request.h"

#include <assert.h>
#include <ctype.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/* 복사본 안의 글자 구간. '\0'이 없어서 길이와 같이 다닌다 (P20) */
typedef struct {
    const char * start;
    size_t length;
} text_view_t;

/* 헤더 줄을 한 번 훑으며 모은 것 (P13) */
typedef struct {
    size_t host_count;
    text_view_t host;
    size_t length_count;
    text_view_t length;
    bool has_transfer_encoding;
    bool has_close;
} header_summary_t;

static const char ABSOLUTE_PREFIX[] = "http://";

static size_t pick_smaller (size_t first, size_t second);
static bool is_broken_line_end (const char * buffer, size_t scan_length,
                                size_t i);
static bool is_header_end_at (const char * buffer, size_t i);
static char * copy_bytes (const char * source, size_t length);
static int parse_request_parts (const char * head, size_t head_length,
                                size_t max_body, request_t * parsed);
static size_t find_line_end (const char * text, size_t length,
                             size_t start);
static int parse_request_line (const char * line, size_t length,
                               request_t * parsed);
static bool split_request_line (const char * line, size_t length,
                                text_view_t * parts);
static bool find_space (const char * text, size_t length, size_t start,
                        size_t * position);
static bool parse_method (text_view_t method, method_t * result);
static bool is_view_equal (text_view_t view, const char * expected);
static bool parse_target (text_view_t target, request_t * parsed);
static bool parse_absolute_target (text_view_t target, request_t * parsed);
static void set_header_area (const char * head, size_t head_length,
                             size_t line_end, request_t * parsed);
static bool collect_header_summary (const char * headers, size_t length,
                                    header_summary_t * summary);
static void add_header_to_summary (const header_view_t * line,
                                   header_summary_t * summary);
static bool is_name_equal (const header_view_t * line, const char * name);
static bool is_equal_ignoring_case (const char * text, size_t length,
                                    const char * lower);
static bool has_close_token (const char * value, size_t length);
static bool is_close_token (const char * token, size_t length);
static text_view_t trim_view (const char * start, size_t length);
static bool is_blank (char letter);
static int check_host (const header_summary_t * summary,
                       request_t * parsed);
static int parse_content_length (const header_summary_t * summary,
                                 size_t max_body, request_t * parsed);
static bool is_all_digits (text_view_t value);
static bool read_limited_number (text_view_t value, size_t limit,
                                 size_t * number);
static void commit_parsed_fields (request_t * request,
                                  const request_t * parsed);
static body_result start_discard (request_t * request, char * buffer,
                                  size_t * buffer_length);
static body_result decide_body_result (size_t remaining);
static body_result start_collect (request_t * request, char * buffer,
                                  size_t * buffer_length);

/* ── 공개 함수 ── */

/* (P3, P17) */
bool skip_leading_empty_lines (char * buffer, size_t * length) {
    size_t count = 0;

    while (count + 1 < *length && buffer[count] == '\r'
           && buffer[count + 1] == '\n') {
        count += 2;
    }
    if (count < *length && buffer[count] == '\n') {
        // \r이 먼저 오는 건 덜 온 경우 일 수 있기 때문에 여기서는 \n만 잡음
        return false;
    }
    remove_buffer_prefix(buffer, length, count);
    return true;
}

/* (R1c, R4b, B4, B29, P16, P29, P30) 검사 순서 400 → 414 → 431 (B32) */
find_result find_header_end (const char * buffer, size_t length,
                             size_t * header_length, int * status) {
    size_t scan_length = pick_smaller(length, MAX_HEADER_SIZE);
    bool has_first_line = false;

    for (size_t i = 0; i < scan_length; i++) {
        if (is_broken_line_end(buffer, scan_length, i)) {
            *status = 400;
            return FIND_BAD;
        }
        if (buffer[i] == '\n') {
            has_first_line = true;
        }
        if (is_header_end_at(buffer, i)) {
            *header_length = i + 1;
            return FIND_FOUND;
        }
    }
    if (scan_length < MAX_HEADER_SIZE) {
        return FIND_NEED_MORE;
    }
    *status = has_first_line ? 431 : 414;
    return FIND_BAD;
}

/* (P13, P14, P25) */
bool copy_request_head (request_t * request, const char * buffer,
                        size_t header_length) {
    char * head = copy_bytes(buffer, header_length);
    if (head == NULL) {
        return false;
    }
    request->head = head;
    request->head_length = header_length;
    return true;
}

/* (R1b, R1c, G23) 1단계 기본 상한 MAX_BODY_SIZE로 부르는 감싸개 */
parse_result parse_request_head (const char * head, size_t head_length,
                                 request_t * request) {
    return parse_request_head_limited(head, head_length, MAX_BODY_SIZE,
                                      request);
}

/* (R1b, R1c, R4a, B32, G9, G23) BAD에서 status 외 필드를 건드리지 않으려고
 * 따로 모은 뒤 성공했을 때만 옮긴다 */
parse_result parse_request_head_limited (const char * head,
                                         size_t head_length, size_t max_body,
                                         request_t * request) {
    request_t parsed = {0};
    int status = parse_request_parts(head, head_length, max_body, &parsed);
    if (status != 0) {
        request->status = status;
        return PARSE_BAD;
    }
    commit_parsed_fields(request, &parsed);
    return PARSE_OK;
}

/* (P5, P6, P13, P15) */
line_result next_header_line (const char * block, size_t length,
                              size_t * position, header_view_t * line) {
    if (*position >= length) {
        return LINE_END;
    }
    const char * start = block + *position;
    size_t end = find_line_end(block, length, *position);
    size_t line_length = end - *position;
    const char * colon = memchr(start, ':', line_length);
    if (is_blank(start[0]) || colon == NULL) {
        return LINE_BAD;
    }
    size_t name_length = (size_t) (colon - start);
    if (name_length == 0 || is_blank(colon[-1])) {
        return LINE_BAD;
    }
    text_view_t value = trim_view(colon + 1, line_length - name_length - 1);

    line->name = start;
    line->name_length = name_length;
    line->value = value.start;
    line->value_length = value.length;
    *position = pick_smaller(end + 2, length);
    return LINE_OK;
}

/* (R2a, P10, P11, P26) */
body_result start_body (request_t * request, char * buffer,
                        size_t * buffer_length) {
    if (request->content_length == 0) {
        return BODY_DONE;
    }
    if (request->method == METHOD_GET) {
        return start_discard(request, buffer, buffer_length);
    }
    return start_collect(request, buffer, buffer_length);
}

/* (L20) 메서드와 관계없이 할당 없이 버린다 (라우팅 결과가 정적일 때) */
body_result start_discard_body (request_t * request, char * buffer,
                                size_t * buffer_length) {
    return start_discard(request, buffer, buffer_length);
}

/* (G33) 프록시로 가는 요청은 GET 본문도 모은다 */
body_result start_collect_body (request_t * request, char * buffer,
                                size_t * buffer_length) {
    if (request->content_length == 0) {
        return BODY_DONE;
    }
    return start_collect(request, buffer, buffer_length);
}

/* (R3c, P10, P26, L20, L25) 버리는 중인지는 discard_remaining으로 본다.
 * count가 남은 양보다 크면 소켓 코드 버그라 assert로 멈춘다 */
body_result add_body_bytes (request_t * request, size_t count) {
    if (request->discard_remaining > 0) {
        assert(count <= request->discard_remaining);
        request->discard_remaining -= pick_smaller(count,
                                                   request->discard_remaining);
        return decide_body_result(request->discard_remaining);
    }
    size_t remaining = request->content_length - request->body_received;
    assert(count <= remaining);
    request->body_received += pick_smaller(count, remaining);
    return decide_body_result(request->content_length
                              - request->body_received);
}

/* (P27) */
void remove_buffer_prefix (char * buffer, size_t * length, size_t count) {
    if (count >= *length) {
        *length = 0;
        return;
    }
    memmove(buffer, buffer + count, *length - count);
    *length -= count;
}

/* (P24) 전부 0으로 돌려서 두 번째 호출은 free(NULL)이 된다 */
void destroy_request (request_t * request) {
    free(request->head);
    free(request->body);
    *request = (request_t) {0};
}

/* ── 도우미 함수 ── */

/* (P11, P29) */
static size_t pick_smaller (size_t first, size_t second) {
    return first < second ? first : second;
}

/* (B29, P30) \r 없는 \n, 또는 \n이 따라오지 않는 \r.
 * 검사 구간 끝의 \r은 다음 바이트를 아직 모르니 깨진 것으로 보지 않는다 */
static bool is_broken_line_end (const char * buffer, size_t scan_length,
                                size_t i) {
    if (buffer[i] == '\n') {
        return i == 0 || buffer[i - 1] != '\r';
    }
    if (buffer[i] == '\r') {
        return i + 1 < scan_length && buffer[i + 1] != '\n';
    }
    return false;
}

/* (R1c) i는 빈 줄의 마지막 \n 자리 */
static bool is_header_end_at (const char * buffer, size_t i) {
    return i >= 3 && memcmp(buffer + i - 3, "\r\n\r\n", 4) == 0;
}

/* (P14) 반환: 실패하면 NULL
 * length 0은 지금 흐름에선 오지 않지만(FOUND면 최소 \r\n\r\n) 방어 코드로
 * 1바이트를 잡는다. malloc(0)이 NULL을 돌려주는 환경에서 할당 실패로
 * 오해하지 않게 하려는 것 */
static char * copy_bytes (const char * source, size_t length) {
    char * copy = malloc(length > 0 ? length : 1);
    if (copy == NULL) {
        return NULL;
    }
    memcpy(copy, source, length);
    return copy;
}

/* (R1b, R1c, B32, P18) 명세 5번 검사 순서 그대로.
 * 반환: 0 = 성공, 아니면 응답 코드 */
static int parse_request_parts (const char * head, size_t head_length,
                                size_t max_body, request_t * parsed) {
    header_summary_t summary = {0};
    size_t line_end = find_line_end(head, head_length, 0);

    int status = parse_request_line(head, line_end, parsed);
    if (status != 0) {
        return status;
    }
    set_header_area(head, head_length, line_end, parsed);
    if (!collect_header_summary(parsed->headers, parsed->headers_length,
                                &summary)) {
        return 400;
    }
    status = check_host(&summary, parsed);
    if (status != 0) {
        return status;
    }
    if (summary.has_transfer_encoding) {
        return 501;
    }
    status = parse_content_length(&summary, max_body, parsed);
    if (status != 0) {
        return status;
    }
    parsed->keep_alive = !summary.has_close;
    return 0;
}

/* (R1c) 반환: start부터 처음 나오는 \r\n의 위치, 없으면 length */
static size_t find_line_end (const char * text, size_t length,
                             size_t start) {
    for (size_t i = start; i + 1 < length; i++) {
        if (text[i] == '\r' && text[i + 1] == '\n') {
            return i;
        }
    }
    return length;
}

/* (R1b, B1, B5, P1, P2, P4) 반환: 0 = 성공, 아니면 응답 코드 */
static int parse_request_line (const char * line, size_t length,
                               request_t * parsed) {
    text_view_t parts[3];

    if (!split_request_line(line, length, parts)) {
        return 400;
    }
    if (!parse_method(parts[0], &parsed->method)) {
        return 501;
    }
    if (!parse_target(parts[1], parsed)) {
        return 400;
    }
    if (!is_view_equal(parts[2], "HTTP/1.1")) {
        return 505;
    }
    parsed->version = parts[2].start;
    parsed->version_length = parts[2].length;
    return 0;
}

/* (P2) 공백 하나로 나뉜, 빈칸 없는 정확히 세 칸 */
static bool split_request_line (const char * line, size_t length,
                                text_view_t * parts) {
    size_t first_space = 0;
    size_t second_space = 0;
    size_t extra_space = 0;

    if (!find_space(line, length, 0, &first_space)
        || !find_space(line, length, first_space + 1, &second_space)
        || find_space(line, length, second_space + 1, &extra_space)) {
        return false;
    }
    parts[0] = (text_view_t) {line, first_space};
    parts[1] = (text_view_t) {line + first_space + 1,
                              second_space - first_space - 1};
    parts[2] = (text_view_t) {line + second_space + 1,
                              length - second_space - 1};
    return parts[0].length > 0 && parts[1].length > 0
           && parts[2].length > 0;
}

/* (P2) */
static bool find_space (const char * text, size_t length, size_t start,
                        size_t * position) {
    for (size_t i = start; i < length; i++) {
        if (text[i] == ' ') {
            *position = i;
            return true;
        }
    }
    return false;
}

/* (B5, P1) 대소문자를 구분한다 */
static bool parse_method (text_view_t method, method_t * result) {
    if (is_view_equal(method, "GET")) {
        *result = METHOD_GET;
        return true;
    }
    if (is_view_equal(method, "POST")) {
        *result = METHOD_POST;
        return true;
    }
    return false;
}

/* (R1b, B1) 대소문자를 구분한다 */
static bool is_view_equal (text_view_t view, const char * expected) {
    return view.length == strlen(expected)
           && memcmp(view.start, expected, view.length) == 0;
}

/* (P4, P12) 경로는 원문 그대로 */
static bool parse_target (text_view_t target, request_t * parsed) {
    if (target.start[0] == '/') {
        parsed->path = target.start;
        parsed->path_length = target.length;
        return true;
    }
    return parse_absolute_target(target, parsed);
}

/* (P4, P23) http://호스트[:포트]/경로. 호스트는 포트까지 담는다 */
static bool parse_absolute_target (text_view_t target, request_t * parsed) {
    size_t prefix_length = strlen(ABSOLUTE_PREFIX);

    if (target.length <= prefix_length
        || memcmp(target.start, ABSOLUTE_PREFIX, prefix_length) != 0) {
        return false;
    }
    const char * host = target.start + prefix_length;
    size_t rest_length = target.length - prefix_length;
    const char * slash = memchr(host, '/', rest_length);
    if (slash == NULL || slash == host) {
        return false;
    }
    parsed->host = host;
    parsed->host_length = (size_t) (slash - host);
    parsed->path = slash;
    parsed->path_length = rest_length - parsed->host_length;
    return true;
}

/* (P22) head는 find_header_end가 찾은 그대로라 \r\n\r\n으로 끝난다.
 * 헤더 줄이 하나도 없으면 길이 0 */
static void set_header_area (const char * head, size_t head_length,
                             size_t line_end, request_t * parsed) {
    size_t start = pick_smaller(line_end + 2, head_length);
    size_t end = head_length >= 2 ? head_length - 2 : 0;

    parsed->headers = head + start;
    parsed->headers_length = end > start ? end - start : 0;
}

/* (P5, P13) 반환: false = 형식이 틀린 줄이 있음 (400) */
static bool collect_header_summary (const char * headers, size_t length,
                                    header_summary_t * summary) {
    size_t position = 0;
    header_view_t line;
    line_result result;

    while ((result = next_header_line(headers, length, &position, &line))
           == LINE_OK) {
        add_header_to_summary(&line, summary);
    }
    return result == LINE_END;
}

/* (B2, B3, B30, P8, P33) 중복은 개수로 나중에 판단한다 */
static void add_header_to_summary (const header_view_t * line,
                                   header_summary_t * summary) {
    if (is_name_equal(line, "host")) {
        summary->host_count++;
        summary->host = (text_view_t) {line->value, line->value_length};
    }
    if (is_name_equal(line, "content-length")) {
        summary->length_count++;
        summary->length = (text_view_t) {line->value, line->value_length};
    }
    if (is_name_equal(line, "transfer-encoding")) {
        summary->has_transfer_encoding = true;
    }
    if (is_name_equal(line, "connection")
        && has_close_token(line->value, line->value_length)) {
        summary->has_close = true;
    }
}

/* (R1c) name은 소문자로 준다 */
static bool is_name_equal (const header_view_t * line, const char * name) {
    return is_equal_ignoring_case(line->name, line->name_length, name);
}

/* (R1c, P8) lower는 소문자로 준다 */
static bool is_equal_ignoring_case (const char * text, size_t length,
                                    const char * lower) {
    if (length != strlen(lower)) {
        return false;
    }
    for (size_t i = 0; i < length; i++) {
        if (tolower((unsigned char) text[i]) != lower[i]) {
            return false;
        }
    }
    return true;
}

/* (P8) 쉼표로 나눈 값 중 하나라도 close면 참 */
static bool has_close_token (const char * value, size_t length) {
    size_t start = 0;

    for (size_t i = 0; i <= length; i++) {
        if (i < length && value[i] != ',') {
            continue;
        }
        if (is_close_token(value + start, i - start)) {
            return true;
        }
        start = i + 1;
    }
    return false;
}

/* (P8) */
static bool is_close_token (const char * token, size_t length) {
    text_view_t trimmed = trim_view(token, length);

    return is_equal_ignoring_case(trimmed.start, trimmed.length, "close");
}

/* (P6) */
static text_view_t trim_view (const char * start, size_t length) {
    while (length > 0 && is_blank(start[0])) {
        start++;
        length--;
    }
    while (length > 0 && is_blank(start[length - 1])) {
        length--;
    }
    return (text_view_t) {start, length};
}

/* (P5, P6) */
static bool is_blank (char letter) {
    return letter == ' ' || letter == '\t';
}

/* (B2, P4, P23, P32) 절대 주소 형식이어도 Host 헤더는 정확히 하나 필요하다
 * (RFC 9112 3.2). 반환: 0 = 성공, 아니면 응답 코드 */
static int check_host (const header_summary_t * summary,
                       request_t * parsed) {
    if (summary->host_count != 1 || summary->host.length == 0) {
        return 400;
    }
    if (parsed->host != NULL) {
        return 0;
    }
    parsed->host = summary->host.start;
    parsed->host_length = summary->host.length;
    return 0;
}

/* (B3, B4, B30, P9, P10, P31) 반환: 0 = 성공, 아니면 응답 코드 */
static int parse_content_length (const header_summary_t * summary,
                                 size_t max_body, request_t * parsed) {
    if (summary->length_count > 1) {
        return 400;
    }
    if (summary->length_count == 0) {
        return parsed->method == METHOD_POST ? 411 : 0;
    }
    if (!is_all_digits(summary->length)) {
        return 400;
    }
    if (!read_limited_number(summary->length, max_body,
                             &parsed->content_length)) {
        return 413;
    }
    return 0;
}

/* (P9, P31) 빈 값은 숫자가 아니다 */
static bool is_all_digits (text_view_t value) {
    if (value.length == 0) {
        return false;
    }
    for (size_t i = 0; i < value.length; i++) {
        if (!isdigit((unsigned char) value.start[i])) {
            return false;
        }
    }
    return true;
}

/* (B4, P9) 상한을 넘는 순간 멈춰서 넘침으로 작은 값이 되지 않게 한다.
 * 반환: false = limit 초과 */
static bool read_limited_number (text_view_t value, size_t limit,
                                 size_t * number) {
    size_t total = 0;

    for (size_t i = 0; i < value.length; i++) {
        total = total * 10 + (size_t) (value.start[i] - '0');
        if (total > limit) {
            return false;
        }
    }
    *number = total;
    return true;
}

/* (R1b, R1c) 소유 필드(head·body)는 건드리지 않는다 */
static void commit_parsed_fields (request_t * request,
                                  const request_t * parsed) {
    request->method = parsed->method;
    request->path = parsed->path;
    request->path_length = parsed->path_length;
    request->host = parsed->host;
    request->host_length = parsed->host_length;
    request->version = parsed->version;
    request->version_length = parsed->version_length;
    request->headers = parsed->headers;
    request->headers_length = parsed->headers_length;
    request->content_length = parsed->content_length;
    request->keep_alive = parsed->keep_alive;
}

/* (P10) GET 본문은 할당 없이 버리고 다음 요청 경계는 남긴다 */
static body_result start_discard (request_t * request, char * buffer,
                                  size_t * buffer_length) {
    size_t taken = pick_smaller(*buffer_length, request->content_length);

    remove_buffer_prefix(buffer, buffer_length, taken);
    request->discard_remaining = request->content_length - taken;
    return decide_body_result(request->discard_remaining);
}

/* (P26) */
static body_result decide_body_result (size_t remaining) {
    return remaining == 0 ? BODY_DONE : BODY_NEED_MORE;
}

/* (R2a, P11) 할당을 먼저 끝내서 실패해도 연결 버퍼는 그대로 남는다 */
static body_result start_collect (request_t * request, char * buffer,
                                  size_t * buffer_length) {
    char * body = malloc(request->content_length);
    if (body == NULL) {
        return BODY_NO_MEMORY;
    }
    size_t taken = pick_smaller(*buffer_length, request->content_length);

    memcpy(body, buffer, taken);
    remove_buffer_prefix(buffer, buffer_length, taken);
    request->body = body;
    request->body_received = taken;
    return decide_body_result(request->content_length - taken);
}
