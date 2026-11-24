#include "proxy.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static void check_routes(void);
static void check_fragmented_reply(void);

/* ── 공개 함수 ── */

int main (void) {
    check_routes();
    check_fragmented_reply();
    puts("proxy unit checks passed");
    return 0;
}

/* ── 도우미 함수 ── */

static void check_routes (void) {
    proxy_route_t route = {0};
    assert(find_backend_path(&route, "/edge") == NULL);
    assert(set_proxy_route(&route, "/edge", "127.0.0.1", "8081"));
    assert(strcmp(find_backend_path(&route, "/edge"), "/") == 0);
    assert(strcmp(find_backend_path(&route, "/edge/a/b"), "/a/b") == 0);
    assert(find_backend_path(&route, "/edgecase") == NULL);
    assert(!set_proxy_route(&route, "/edge", "127.0.0.1", "65536"));
}

static void check_fragmented_reply (void) {
    int pair[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    proxy_t proxy = {.socket_fd = pair[0], .reading = true,
                     .expected = SIZE_MAX, .deadline = monotonic_ms() + 3000};
    response_t response = {.file = -1};
    const char first[] = "HTTP/1.1 201 Created\r\nContent-Length: 3\r\n\r";
    assert(write(pair[1], first, sizeof(first) - 1) == sizeof(first) - 1);
    assert(send_request_or_read_response(&proxy, NULL, NULL, &response) == 0);
    assert(write(pair[1], "\nx", 2) == 2);
    assert(send_request_or_read_response(&proxy, NULL, NULL, &response) == 0);
    assert(write(pair[1], "\0z", 2) == 2);
    assert(send_request_or_read_response(&proxy, NULL, NULL, &response) == 1);
    assert(response.remaining == 3);
    assert(memcmp(response.body, "x\0z", 3) == 0);
    assert(strstr(response.header, "HTTP/1.1 201 Created\r\n") != NULL);
    free(proxy.received);
    close(pair[0]);
    close(pair[1]);
}
