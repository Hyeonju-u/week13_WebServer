#ifndef PROXY_H
#define PROXY_H

#include "http_parser.h"
#include "server.h"

void start_proxy_request (server_t * server, conn_t * client,
    const http_request_t * request, route_t * route);
void handle_backend_event (server_t * server, conn_t * backend,
    const event_t * event);
void handle_backend_timeout (server_t * server, conn_t * backend,
    deadline_kind_t kind);
void compute_backend_interest (const conn_t * backend, int * wants_read,
    int * wants_write);

#endif
