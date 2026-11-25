#include "proxy_message.h"

#include <string.h>
#include <strings.h>

#include "http_response.h"

#define NOT_FOUND ((size_t) -1)
#define STATUS_LINE_MIN 12

/* 손님과 프록시 구간에서만 뜻이 있는 헤더들 (PRX-10) */
static const char * const hop_by_hop_names[] = {
    "Connection", "Keep-Alive", "Proxy-Connection", "TE", "Trailer",
    "Upgrade",
};

static const char * get_method_name (http_method_t method);
static int is_dropped_request_header (const char * bytes,
    const http_request_t * request, const header_field_t * field);
static int is_hop_by_hop_header (const char * bytes,
    const header_block_t * block, const header_field_t * field);
static int append_header_line (byte_buffer_t * output, const char * bytes,
    const header_field_t * field);
static size_t find_status_line_end (const char * bytes, size_t length);
static int parse_status_line (const char * bytes, size_t line_length,
    backend_response_t * response);
static int read_framing_headers (const char * bytes,
    backend_response_t * response);

/* ── 공개 함수 ── */

/*
 * 백엔드 연결마다 새로 만들지 않고 손님 쪽에 한 번 만들어 둔다.
 * 연결에 실패해 다음 백엔드로 넘어갈 때 그대로 다시 보내기 위해서다 (PRX-09).
 */
int build_forward_request (byte_buffer_t * forward,
    const http_request_t * request, const char * bytes,
    const char * client_address) {
    if (append_format(forward, "%s %.*s HTTP/1.1\r\n",
            get_method_name(request->method), (int) request->uri_length,
            bytes + request->uri_offset) != 0) {
        return -1;
    }
    for (size_t i = 0; i < request->headers.count; i++) {
        const header_field_t * field = &request->headers.fields[i];
        if (is_dropped_request_header(bytes, request, field)) {
            continue;
        }
        if (append_header_line(forward, bytes, field) != 0) {
            return -1;
        }
    }
    if (append_format(forward, "Connection: close\r\n"
            "X-Forwarded-For: %s\r\n\r\n", client_address) != 0) {
        return -1;
    }
    return append_bytes(forward, bytes + request->head_length,
        request->body_length);
}

/* 1xx도 파싱만 하고, 버릴지 말지는 호출자가 정한다 */
parse_result_t parse_backend_response (const char * bytes, size_t length,
    backend_response_t * response) {
    memset(response, 0, sizeof(*response));
    size_t line_end = find_status_line_end(bytes, length);
    if (line_end == NOT_FOUND) {
        return length >= RESPONSE_HEADER_MAX ? PARSE_ERROR : PARSE_NEED_MORE;
    }
    if (parse_status_line(bytes, line_end, response) != 0) {
        return PARSE_ERROR;
    }
    int ignored_status = 0;
    parse_result_t result = parse_header_block(bytes, length, line_end + 2,
        RESPONSE_HEADER_MAX, &response->headers, &ignored_status);
    if (result != PARSE_COMPLETE) {
        return result;
    }
    if (read_framing_headers(bytes, response) != 0) {
        return PARSE_ERROR;
    }
    response->head_length = response->headers.end_offset;
    return PARSE_COMPLETE;
}

/*
 * Transfer-Encoding은 지우지 않는다. 본문을 그대로 넘기므로 지우면 손님이
 * 본문을 잘못 해석한다 (PRX-13).
 */
int append_relayed_head (byte_buffer_t * output, const char * bytes,
    const backend_response_t * response, int keep_alive) {
    int status_code = response->status_code;
    int result;
    if (response->reason_length > 0) {
        result = append_format(output, "HTTP/1.1 %d %.*s\r\n", status_code,
            (int) response->reason_length, bytes + response->reason_offset);
    } else {
        result = append_format(output, "HTTP/1.1 %d %s\r\n", status_code,
            get_status_text(status_code));
    }
    if (result != 0) {
        return -1;
    }
    for (size_t i = 0; i < response->headers.count; i++) {
        const header_field_t * field = &response->headers.fields[i];
        if (is_hop_by_hop_header(bytes, &response->headers, field)) {
            continue;
        }
        if (append_header_line(output, bytes, field) != 0) {
            return -1;
        }
    }
    return append_format(output, "Connection: %s\r\n\r\n",
        keep_alive ? "keep-alive" : "close");
}

/* ── 도우미 함수 ── */

static const char * get_method_name (http_method_t method) {
    if (method == METHOD_GET) {
        return "GET";
    }
    if (method == METHOD_HEAD) {
        return "HEAD";
    }
    return "POST";
}

/* Expect는 서버가 이미 처리했고, X-Forwarded-For는 손님이 속일 수 있다 */
static int is_dropped_request_header (const char * bytes,
    const http_request_t * request, const header_field_t * field) {
    if (is_header_named(bytes, field, "Expect")
        || is_header_named(bytes, field, "X-Forwarded-For")) {
        return 1;
    }
    return is_hop_by_hop_header(bytes, &request->headers, field);
}

static int is_hop_by_hop_header (const char * bytes,
    const header_block_t * block, const header_field_t * field) {
    size_t count = sizeof(hop_by_hop_names) / sizeof(hop_by_hop_names[0]);
    for (size_t i = 0; i < count; i++) {
        if (is_header_named(bytes, field, hop_by_hop_names[i])) {
            return 1;
        }
    }
    return is_connection_listed(bytes, block, bytes + field->name_offset,
        field->name_length);
}

static int append_header_line (byte_buffer_t * output, const char * bytes,
    const header_field_t * field) {
    return append_format(output, "%.*s: %.*s\r\n", (int) field->name_length,
        bytes + field->name_offset, (int) field->value_length,
        bytes + field->value_offset);
}

static size_t find_status_line_end (const char * bytes, size_t length) {
    size_t limit = length < RESPONSE_HEADER_MAX ? length
        : RESPONSE_HEADER_MAX;
    for (size_t i = 0; i + 1 < limit; i++) {
        if (bytes[i] == '\r' && bytes[i + 1] == '\n') {
            return i;
        }
    }
    return NOT_FOUND;
}

/* "HTTP/1.x 코드" 뒤에는 공백과 사유 문구가 올 수 있다 */
static int parse_status_line (const char * bytes, size_t line_length,
    backend_response_t * response) {
    if (line_length < STATUS_LINE_MIN || memcmp(bytes, "HTTP/1.", 7) != 0
        || bytes[7] < '0' || bytes[7] > '9' || bytes[8] != ' ') {
        return -1;
    }
    for (size_t i = 9; i < 12; i++) {
        if (bytes[i] < '0' || bytes[i] > '9') {
            return -1;
        }
    }
    response->status_code = (bytes[9] - '0') * 100 + (bytes[10] - '0') * 10
        + (bytes[11] - '0');
    if (response->status_code < 100 || response->status_code > 599) {
        return -1;
    }
    if (line_length > STATUS_LINE_MIN && bytes[12] != ' ') {
        return -1;
    }
    if (line_length > STATUS_LINE_MIN + 1) {
        response->reason_offset = STATUS_LINE_MIN + 1;
        response->reason_length = line_length - STATUS_LINE_MIN - 1;
    }
    return 0;
}

/* 둘이 함께 오거나 길이가 어긋나면 응답 경계를 믿을 수 없다 (NFR-03) */
static int read_framing_headers (const char * bytes,
    backend_response_t * response) {
    size_t length = 0;
    int result = parse_content_length(bytes, &response->headers, &length);
    if (result < 0) {
        return -1;
    }
    response->has_content_length = result == 1;
    response->content_length = length;
    response->has_transfer_encoding = find_header_index(bytes,
        &response->headers, "Transfer-Encoding", 0) >= 0;
    if (response->has_transfer_encoding && response->has_content_length) {
        return -1;
    }
    return 0;
}
