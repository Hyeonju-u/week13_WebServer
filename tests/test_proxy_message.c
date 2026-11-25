#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "http_parser.h"
#include "proxy_message.h"

static char * build_text (const char * request_text) {
    http_request_t request;
    int status_code = 0;
    assert(parse_request(request_text, strlen(request_text), &request,
        &status_code) == PARSE_COMPLETE);
    byte_buffer_t forward;
    init_buffer(&forward);
    assert(build_forward_request(&forward, &request, request_text,
        "10.1.2.3") == 0);
    char * text = malloc(forward.length + 1);
    memcpy(text, forward.data, forward.length);
    text[forward.length] = '\0';
    release_buffer(&forward);
    return text;
}

static int count_text (const char * text, const char * pattern) {
    int count = 0;
    for (const char * found = strstr(text, pattern); found != NULL;
        found = strstr(found + 1, pattern)) {
        count++;
    }
    return count;
}

static void test_forward_request (void) {
    char * text = build_text("POST /api/users?id=3 HTTP/1.1\r\n"
        "Host: example.com\r\nConnection: keep-alive, X-Secret\r\n"
        "Keep-Alive: timeout=5\r\nX-Secret: hidden\r\n"
        "X-Forwarded-For: 6.6.6.6\r\nExpect: 100-continue\r\n"
        "Content-Length: 4\r\nX-Custom: yes\r\n\r\nbody");
    assert(strncmp(text, "POST /api/users?id=3 HTTP/1.1\r\n", 31) == 0);
    assert(strstr(text, "Host: example.com\r\n") != NULL);
    assert(strstr(text, "X-Custom: yes\r\n") != NULL);
    assert(strstr(text, "Content-Length: 4\r\n") != NULL);
    assert(count_text(text, "Connection:") == 1);
    assert(strstr(text, "Connection: close\r\n") != NULL);
    assert(strstr(text, "Keep-Alive") == NULL);
    assert(strstr(text, "X-Secret") == NULL);
    assert(strstr(text, "Expect") == NULL);
    assert(strstr(text, "6.6.6.6") == NULL);
    assert(count_text(text, "X-Forwarded-For:") == 1);
    assert(strstr(text, "X-Forwarded-For: 10.1.2.3\r\n") != NULL);
    assert(strcmp(text + strlen(text) - 8, "\r\n\r\nbody") == 0);
    free(text);
}

static parse_result_t parse_response_text (const char * text,
    backend_response_t * response) {
    return parse_backend_response(text, strlen(text), response);
}

static void test_parse_response (void) {
    backend_response_t response;
    const char * ok = "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello";
    assert(parse_response_text(ok, &response) == PARSE_COMPLETE);
    assert(response.status_code == 200);
    assert(response.has_content_length == 1 && response.content_length == 5);
    assert(response.reason_length == 2);
    assert(response.head_length == strlen(ok) - 5);
    assert(parse_response_text("HTTP/1.1 100 Continue\r\n\r\n", &response)
        == PARSE_COMPLETE);
    assert(response.status_code == 100);
    assert(parse_response_text("HTTP/1.1 204\r\n\r\n", &response)
        == PARSE_COMPLETE);
    assert(response.reason_length == 0);
    assert(parse_response_text("HTTP/1.1 200 OK\r\nContent-Le", &response)
        == PARSE_NEED_MORE);
    assert(parse_response_text("HTTP/1.1 20", &response) == PARSE_NEED_MORE);
}

static void test_bad_responses (void) {
    backend_response_t response;
    assert(parse_response_text("garbage\r\n\r\n", &response) == PARSE_ERROR);
    assert(parse_response_text("HTTP/1.1 abc OK\r\n\r\n", &response)
        == PARSE_ERROR);
    assert(parse_response_text("HTTP/1.1 700 X\r\n\r\n", &response)
        == PARSE_ERROR);
    assert(parse_response_text("HTTP/1.1 200 OK\r\nContent-Length: 1\r\n"
        "Content-Length: 2\r\n\r\n", &response) == PARSE_ERROR);
    assert(parse_response_text("HTTP/1.1 200 OK\r\nContent-Length: 1\r\n"
        "Transfer-Encoding: chunked\r\n\r\n", &response) == PARSE_ERROR);
    assert(parse_response_text("HTTP/1.1 200 OK\r\nbroken line\r\n\r\n",
        &response) == PARSE_ERROR);
}

static void test_relayed_head (void) {
    const char * text = "HTTP/1.1 200 OK\r\nConnection: close, X-Hop\r\n"
        "X-Hop: 1\r\nKeep-Alive: x\r\nTransfer-Encoding: chunked\r\n"
        "Content-Type: text/plain\r\n\r\n";
    backend_response_t response;
    assert(parse_response_text(text, &response) == PARSE_COMPLETE);
    byte_buffer_t output;
    init_buffer(&output);
    assert(append_relayed_head(&output, text, &response, 1) == 0);
    char * result = malloc(output.length + 1);
    memcpy(result, output.data, output.length);
    result[output.length] = '\0';
    assert(strncmp(result, "HTTP/1.1 200 OK\r\n", 17) == 0);
    assert(strstr(result, "Transfer-Encoding: chunked\r\n") != NULL);
    assert(strstr(result, "Content-Type: text/plain\r\n") != NULL);
    assert(strstr(result, "X-Hop") == NULL);
    assert(strstr(result, "Keep-Alive") == NULL);
    assert(count_text(result, "Connection:") == 1);
    assert(strstr(result, "Connection: keep-alive\r\n\r\n") != NULL);
    free(result);
    release_buffer(&output);
}

int main (void) {
    test_forward_request();
    test_parse_response();
    test_bad_responses();
    test_relayed_head();
    printf("test_proxy_message: 모두 통과\n");
    return 0;
}
