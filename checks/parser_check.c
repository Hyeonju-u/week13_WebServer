#include "http.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void check_status(const char * input, int expected);

/* ── 공개 함수 ── */

int main (void) {
    char input[] = "GET /page?q=one HTTP/1.1\r\nhost: [::1]:8080\r\n\r\n";
    size_t length = strlen(input);
    request_t request;
    for (size_t i = 0; i < length; i++) {
        assert(find_header_end(input, i, 0) == 0);
    }
    assert(find_header_end(input, length, length - 4) == length);
    assert(parse_request_headers(input, length, &request) == 0);
    assert(strcmp(request.method, "GET") == 0);
    assert(strcmp(request.path, "/page") == 0);
    assert(strcmp(request.query, "q=one") == 0);
    assert(request.body_length == 0);

    check_status("Host: example.org", 0);
    check_status("User-Agent: check", 400);
    check_status("Host: \t", 400);
    check_status("Host: local host", 400);
    check_status("Host: [::1", 400);
    check_status("Host: localhost:99999", 400);
    check_status("Host: localhost\r\nHost: localhost", 400);
    check_status("Host: x\r\nContent-Length: 1048576", 0);
    check_status("Host: x\r\nContent-Length: 1048577", 413);
    check_status("Host: x\r\nContent-Length: -1", 400);
    check_status("Host: x\r\nContent-Length: 1\r\nContent-Length: 2", 400);
    check_status("Host: x\r\nTransfer-Encoding: chunked", 501);
    check_status("Host: x\r\nTransfer-Encoding: gzip\r\n"
                 "Content-Length: 0", 400);
    puts("parser checks passed");
    return 0;
}

/* ── 도우미 함수 ── */

static void check_status (const char * input, int expected) {
    char headers[512];
    int length = snprintf(headers, sizeof(headers),
                          "GET / HTTP/1.1\r\n%s\r\n\r\n", input);
    request_t request;
    assert(parse_request_headers(headers, (size_t)length,
                                 &request) == expected);
}
