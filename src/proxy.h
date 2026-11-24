#ifndef PROXY_H
#define PROXY_H

#include "response.h"
#include <netinet/in.h>
#include <stdbool.h>

#define PROXY_TIMEOUT_MS 3000

typedef struct {
    const char * prefix;
    const char * host;
    struct sockaddr_in address;
} proxy_route_t;

typedef struct {
    int socket_fd;
    int64_t deadline;
    bool reading;
    char outgoing[HEADER_LIMIT + 512];
    size_t outgoing_length;
    size_t sent;
    char * received;
    size_t used;
    size_t capacity;
    size_t header_end;
    size_t expected;
    char status[96];
    char type[192];
} proxy_t;

bool set_proxy_route(proxy_route_t * route, const char * prefix,
                     const char * host, const char * port);
const char * find_backend_path(const proxy_route_t * route, const char * path);
int64_t monotonic_ms(void);
proxy_t * create_backend_connection(const proxy_route_t * route,
                                    const char * path,
                                    const request_t * request);
int send_request_or_read_response(proxy_t * proxy, const request_t * request,
                                  const char * body, response_t * response);
void destroy_backend_connection(proxy_t * proxy);

#endif
