#include "proxy_request.h"

#include <arpa/inet.h>
#include <string.h>
#include <strings.h>

/* (G6) RFC 9110 §7.6.1 */
static const char * const HOP_BY_HOP_HEADERS[] = {
    "Connection", "Keep-Alive", "Proxy-Connection", "TE", "Trailer",
    "Transfer-Encoding", "Upgrade",
};

/* (G6, G28) 고정 크기 버퍼에 이어 쓰기. 넘치면 is_full만 세우고 멈춘다 */
typedef struct {
    char * buffer;
    size_t length;
    bool is_full;
} head_writer_t;

static void write_request_line (head_writer_t * writer,
                                const request_t * request);
static void append_bytes (head_writer_t * writer, const char * bytes,
                          size_t length);
static void append_text (head_writer_t * writer, const char * text);
static void write_forwarded_headers (head_writer_t * writer,
                                     const request_t * request);
static bool is_forwarded_header (const request_t * request,
                                 const header_view_t * line);
static bool is_name (const header_view_t * line, const char * name);
static bool is_listed_hop_by_hop (const header_view_t * line);
static bool is_named_in_connection (const char * headers,
                                    size_t headers_length,
                                    const header_view_t * line);
static bool has_token (const char * value, size_t length,
                       const header_view_t * line);
static bool is_token_equal (const char * token, size_t length,
                            const header_view_t * line);
static void write_forwarded_for (head_writer_t * writer,
                                 const request_t * request,
                                 const struct sockaddr_in * client);

/* ── 공개 함수 ── */

/* (G3~G6, G28) 요청 줄 → 넘길 헤더 → X-Forwarded-For → Connection: close */
size_t write_backend_request (const request_t * request,
                              const struct sockaddr_in * client,
                              char * buffer) {
    head_writer_t writer = { .buffer = buffer, .length = 0,
                             .is_full = false };

    write_request_line(&writer, request);
    write_forwarded_headers(&writer, request);
    write_forwarded_for(&writer, request, client);
    append_text(&writer, "Connection: close\r\n\r\n");
    return writer.is_full ? 0 : writer.length;
}

/* (G6, G10) */
bool is_hop_by_hop_header (const char * headers, size_t headers_length,
                           const header_view_t * line) {
    return is_listed_hop_by_hop(line)
           || is_named_in_connection(headers, headers_length, line);
}

/* ── 도우미 함수 ── */

/* (G3, G4, G5) 원문 경로(쿼리 포함, 절대 주소면 경로 부분)를 그대로, 1.0으로 */
static void write_request_line (head_writer_t * writer,
                                const request_t * request) {
    append_text(writer, request->method == METHOD_POST ? "POST " : "GET ");
    append_bytes(writer, request->path, request->path_length);
    append_text(writer, " HTTP/1.0\r\n");
}

/* (G6, G28) 넘치면 그 뒤로는 쓰지 않는다 (결과는 0 = 431) */
static void append_bytes (head_writer_t * writer, const char * bytes,
                          size_t length) {
    if (writer->is_full || length > MAX_HEADER_SIZE - writer->length) {
        writer->is_full = true;
        return;
    }
    memcpy(writer->buffer + writer->length, bytes, length);
    writer->length += length;
}

static void append_text (head_writer_t * writer, const char * text) {
    append_bytes(writer, text, strlen(text));
}

/* (G6, P13) Host를 포함한 나머지 헤더는 손님 값 그대로 */
static void write_forwarded_headers (head_writer_t * writer,
                                     const request_t * request) {
    size_t position = 0;
    header_view_t line;

    while (next_header_line(request->headers, request->headers_length,
                            &position, &line) == LINE_OK) {
        if (!is_forwarded_header(request, &line)) {
            continue;
        }
        append_bytes(writer, line.name, line.name_length);
        append_text(writer, ": ");
        append_bytes(writer, line.value, line.value_length);
        append_text(writer, "\r\n");
    }
}

/* (G6, G7) hop-by-hop·Expect는 지우고, X-Forwarded-For는 따로 합쳐 쓴다 */
static bool is_forwarded_header (const request_t * request,
                                 const header_view_t * line) {
    if (is_name(line, "Expect") || is_name(line, "X-Forwarded-For")) {
        return false;
    }
    return !is_hop_by_hop_header(request->headers, request->headers_length,
                                 line);
}

/* (P8) 헤더 이름은 대소문자를 가리지 않는다 */
static bool is_name (const header_view_t * line, const char * name) {
    return line->name_length == strlen(name)
           && strncasecmp(line->name, name, line->name_length) == 0;
}

/* (G6) */
static bool is_listed_hop_by_hop (const header_view_t * line) {
    size_t count = sizeof(HOP_BY_HOP_HEADERS) / sizeof(HOP_BY_HOP_HEADERS[0]);

    for (size_t i = 0; i < count; i++) {
        if (is_name(line, HOP_BY_HOP_HEADERS[i])) {
            return true;
        }
    }
    return false;
}

/* (G6) Connection 줄이 여러 개여도 전부 본다 */
static bool is_named_in_connection (const char * headers,
                                    size_t headers_length,
                                    const header_view_t * line) {
    size_t position = 0;
    header_view_t connection;

    while (next_header_line(headers, headers_length, &position,
                            &connection) == LINE_OK) {
        if (is_name(&connection, "Connection")
            && has_token(connection.value, connection.value_length, line)) {
            return true;
        }
    }
    return false;
}

/* (G6, P8) 쉼표로 나눈 값 중 하나가 line의 이름과 같은지 */
static bool has_token (const char * value, size_t length,
                       const header_view_t * line) {
    size_t start = 0;

    for (size_t i = 0; i <= length; i++) {
        if (i < length && value[i] != ',') {
            continue;
        }
        if (is_token_equal(value + start, i - start, line)) {
            return true;
        }
        start = i + 1;
    }
    return false;
}

/* (G6, P6) 앞뒤 공백·탭을 뺀 뒤 비교한다 */
static bool is_token_equal (const char * token, size_t length,
                            const header_view_t * line) {
    while (length > 0 && (token[0] == ' ' || token[0] == '\t')) {
        token++;
        length--;
    }
    while (length > 0
           && (token[length - 1] == ' ' || token[length - 1] == '\t')) {
        length--;
    }
    return length == line->name_length
           && strncasecmp(token, line->name, length) == 0;
}

/* (G6) 있던 값들을 적힌 순서대로 ", "로 합치고 끝에 손님 IP */
static void write_forwarded_for (head_writer_t * writer,
                                 const request_t * request,
                                 const struct sockaddr_in * client) {
    char address[INET_ADDRSTRLEN];
    size_t position = 0;
    header_view_t line;

    if (inet_ntop(AF_INET, &client->sin_addr, address,
                  sizeof(address)) == NULL) {
        address[0] = '\0';
    }
    append_text(writer, "X-Forwarded-For: ");
    while (next_header_line(request->headers, request->headers_length,
                            &position, &line) == LINE_OK) {
        if (is_name(&line, "X-Forwarded-For") && line.value_length > 0) {
            append_bytes(writer, line.value, line.value_length);
            append_text(writer, ", ");
        }
    }
    append_text(writer, address);
    append_text(writer, "\r\n");
}
