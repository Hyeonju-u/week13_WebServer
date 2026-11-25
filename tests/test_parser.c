#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "http_parser.h"

static parse_result_t parse_text (const char * text, http_request_t * request,
    int * status_code) {
    *status_code = 0;
    return parse_request(text, strlen(text), request, status_code);
}

static void expect_error (const char * text, int expected_status) {
    http_request_t request;
    int status_code = 0;
    parse_result_t result = parse_text(text, &request, &status_code);
    if (result != PARSE_ERROR || status_code != expected_status) {
        fprintf(stderr, "기대 %d, 실제 결과 %d / 코드 %d: %s\n",
            expected_status, (int) result, status_code, text);
        abort();
    }
}

static void test_simple_get (void) {
    http_request_t request;
    int status_code = 0;
    const char * text = "GET /index.html?lang=ko HTTP/1.1\r\n"
        "Host: example.com\r\nUser-Agent:  curl \r\n\r\n";
    assert(parse_text(text, &request, &status_code) == PARSE_COMPLETE);
    assert(request.method == METHOD_GET);
    assert(strcmp(request.path, "/index.html") == 0);
    assert(request.raw_path_length == 11);
    assert(request.uri_length == 19);
    assert(request.keep_alive == 1);
    assert(request.total_length == strlen(text));
    int index = find_header_index(text, &request.headers, "user-agent", 0);
    assert(index >= 0);
    assert(request.headers.fields[index].value_length == 4);
}

/* 1바이트씩 나눠 보내도 한 번에 보낸 것과 같은 결과여야 한다 (REQ-03) */
static void test_split_input (void) {
    const char * text = "POST /x HTTP/1.1\r\nHost: a\r\nContent-Length: 4\r\n"
        "\r\nabcd";
    size_t length = strlen(text);
    for (size_t i = 0; i < length; i++) {
        http_request_t request;
        int status_code = 0;
        assert(parse_request(text, i, &request, &status_code)
            == PARSE_NEED_MORE);
    }
    http_request_t request;
    int status_code = 0;
    assert(parse_request(text, length, &request, &status_code)
        == PARSE_COMPLETE);
    assert(request.body_length == 4);
    assert(request.head_length + request.body_length == length);
}

static void test_pipelined_requests (void) {
    const char * first = "GET /a HTTP/1.1\r\nHost: h\r\n\r\n";
    const char * text = "GET /a HTTP/1.1\r\nHost: h\r\n\r\n"
        "GET /b HTTP/1.1\r\nHost: h\r\n\r\n";
    http_request_t request;
    int status_code = 0;
    assert(parse_text(text, &request, &status_code) == PARSE_COMPLETE);
    assert(request.total_length == strlen(first));
    assert(strcmp(request.path, "/a") == 0);
}

static void test_request_line_errors (void) {
    expect_error("get / HTTP/1.1\r\nHost: a\r\n\r\n", 501);
    expect_error("PUT / HTTP/1.1\r\nHost: a\r\n\r\n", 501);
    expect_error("G(T / HTTP/1.1\r\nHost: a\r\n\r\n", 400);
    expect_error("GET /  HTTP/1.1\r\nHost: a\r\n\r\n", 400);
    expect_error("GET / HTTP/1.1 x\r\nHost: a\r\n\r\n", 400);
    expect_error("GET /\r\nHost: a\r\n\r\n", 400);
    expect_error("GET / HTTP/2.0\r\nHost: a\r\n\r\n", 505);
    expect_error("GET / HTTP/1.2\r\nHost: a\r\n\r\n", 505);
    expect_error("GET / HTTPS/1.1\r\nHost: a\r\n\r\n", 400);
    expect_error("GET index.html HTTP/1.1\r\nHost: a\r\n\r\n", 400);
    expect_error("GET http://a/ HTTP/1.1\r\nHost: a\r\n\r\n", 400);
    expect_error("GET /a\x01 HTTP/1.1\r\nHost: a\r\n\r\n", 400);
}

static void test_long_request_line (void) {
    size_t size = REQUEST_LINE_MAX + 100;
    char * text = malloc(size + 1);
    memset(text, 'a', size);
    memcpy(text, "GET /", 5);
    text[size] = '\0';
    http_request_t request;
    int status_code = 0;
    assert(parse_text(text, &request, &status_code) == PARSE_ERROR);
    assert(status_code == 414);
    free(text);
}

static void test_path_decoding (void) {
    http_request_t request;
    int status_code = 0;
    assert(parse_text("GET /a%20b HTTP/1.1\r\nHost: a\r\n\r\n", &request,
        &status_code) == PARSE_COMPLETE);
    assert(strcmp(request.path, "/a b") == 0);
    assert(parse_text("GET /a+b HTTP/1.1\r\nHost: a\r\n\r\n", &request,
        &status_code) == PARSE_COMPLETE);
    assert(strcmp(request.path, "/a+b") == 0);
    assert(parse_text("GET /%2e%2E/x HTTP/1.1\r\nHost: a\r\n\r\n", &request,
        &status_code) == PARSE_COMPLETE);
    assert(strcmp(request.path, "/../x") == 0);
    assert(parse_text("GET /%252e HTTP/1.1\r\nHost: a\r\n\r\n", &request,
        &status_code) == PARSE_COMPLETE);
    assert(strcmp(request.path, "/%2e") == 0);
    expect_error("GET /%zz HTTP/1.1\r\nHost: a\r\n\r\n", 400);
    expect_error("GET /%4 HTTP/1.1\r\nHost: a\r\n\r\n", 400);
    expect_error("GET /a%00b HTTP/1.1\r\nHost: a\r\n\r\n", 400);
    expect_error("GET /a%2Fb HTTP/1.1\r\nHost: a\r\n\r\n", 400);
    expect_error("GET /a%5cb HTTP/1.1\r\nHost: a\r\n\r\n", 400);
}

static void test_header_errors (void) {
    expect_error("GET / HTTP/1.1\r\n\r\n", 400);
    expect_error("GET / HTTP/1.1\r\nHost: a\r\nHost: b\r\n\r\n", 400);
    expect_error("GET / HTTP/1.1\r\nHost:\r\n\r\n", 400);
    expect_error("GET / HTTP/1.1\r\nHost : a\r\n\r\n", 400);
    expect_error("GET / HTTP/1.1\r\nHost: a\r\nbroken\r\n\r\n", 400);
    expect_error("GET / HTTP/1.1\r\nHost: a\r\n folded\r\n\r\n", 400);
    expect_error("GET / HTTP/1.1\r\nHost: a\r\nX: a\x01" "b\r\n\r\n", 400);
    expect_error("GET / HTTP/1.1\r\nHost: a\r\nX: a\rb\r\n\r\n", 400);
}

static void test_header_limits (void) {
    size_t size = HEADER_BYTES_MAX + 200;
    char * text = malloc(size + 64);
    strcpy(text, "GET / HTTP/1.1\r\nHost: a\r\nX: ");
    size_t used = strlen(text);
    memset(text + used, 'v', size);
    text[used + size] = '\0';
    http_request_t request;
    int status_code = 0;
    assert(parse_text(text, &request, &status_code) == PARSE_ERROR);
    assert(status_code == 431);
    free(text);
    char many[HEADER_COUNT_MAX * 16 + 128];
    strcpy(many, "GET / HTTP/1.1\r\nHost: a\r\n");
    for (int i = 0; i < HEADER_COUNT_MAX + 1; i++) {
        char line[32];
        snprintf(line, sizeof(line), "H%d: v\r\n", i);
        strcat(many, line);
    }
    strcat(many, "\r\n");
    assert(parse_text(many, &request, &status_code) == PARSE_ERROR);
    assert(status_code == 431);
}

static void test_content_length (void) {
    expect_error("POST / HTTP/1.1\r\nHost: a\r\n\r\n", 411);
    expect_error("POST / HTTP/1.1\r\nHost: a\r\nContent-Length: abc\r\n\r\n",
        400);
    expect_error("POST / HTTP/1.1\r\nHost: a\r\nContent-Length: -1\r\n\r\n",
        400);
    expect_error("POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 1 2\r\n\r\n",
        400);
    expect_error("POST / HTTP/1.1\r\nHost: a\r\n"
        "Content-Length: 99999999999999999999999\r\n\r\n", 400);
    expect_error("POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 1\r\n"
        "Content-Length: 2\r\n\r\nab", 400);
    expect_error("POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 2097152\r\n"
        "\r\n", 413);
    http_request_t request;
    int status_code = 0;
    assert(parse_text("POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 2\r\n"
        "Content-Length: 2\r\n\r\nab", &request, &status_code)
        == PARSE_COMPLETE);
}

static void test_transfer_encoding (void) {
    expect_error("POST / HTTP/1.1\r\nHost: a\r\n"
        "Transfer-Encoding: chunked\r\n\r\n", 501);
    expect_error("POST / HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: chunked\r\n"
        "Content-Length: 3\r\n\r\nabc", 400);
}

static void test_connection_header (void) {
    http_request_t request;
    int status_code = 0;
    assert(parse_text("GET / HTTP/1.1\r\nHost: a\r\nConnection: Upgrade, "
        "CLOSE\r\n\r\n", &request, &status_code) == PARSE_COMPLETE);
    assert(request.keep_alive == 0);
    assert(parse_text("GET / HTTP/1.0\r\n\r\n", &request, &status_code)
        == PARSE_COMPLETE);
    assert(request.keep_alive == 0);
    assert(parse_text("GET / HTTP/1.0\r\nConnection: keep-alive\r\n\r\n",
        &request, &status_code) == PARSE_COMPLETE);
    assert(request.keep_alive == 1);
}

static void test_expect_continue (void) {
    http_request_t request;
    int status_code = 0;
    const char * head = "POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 3\r\n"
        "Expect: 100-continue\r\n\r\n";
    assert(parse_text(head, &request, &status_code) == PARSE_NEED_MORE);
    assert(request.has_head == 1);
    assert(request.expect_continue == 1);
    expect_error("POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 3\r\n"
        "Expect: something\r\n\r\n", 417);
}

static void test_binary_body (void) {
    const char text[] = "POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 5\r\n"
        "\r\na\0b\r\n";
    http_request_t request;
    int status_code = 0;
    assert(parse_request(text, sizeof(text) - 1, &request, &status_code)
        == PARSE_COMPLETE);
    assert(request.body_length == 5);
}

int main (void) {
    test_simple_get();
    test_split_input();
    test_pipelined_requests();
    test_request_line_errors();
    test_long_request_line();
    test_path_decoding();
    test_header_errors();
    test_header_limits();
    test_content_length();
    test_transfer_encoding();
    test_connection_header();
    test_expect_continue();
    test_binary_body();
    printf("test_parser: 모두 통과\n");
    return 0;
}
