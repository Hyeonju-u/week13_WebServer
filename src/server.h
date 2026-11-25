#ifndef SERVER_H
#define SERVER_H

#include <stddef.h>

#include "connection.h"
#include "event.h"
#include "proxy_config.h"
#include "timer_heap.h"

typedef struct {
    int listen_fd;
    int reserve_fd;
    int root_fd;
    size_t root_path_length;
    event_loop_t * loop;
    timer_heap_t * heap;
    proxy_config_t * proxy_config;
    size_t max_connections;
    size_t connection_count;
    conn_t * all_head;
    conn_t * pending_head;
    conn_t * pending_tail;
    size_t pending_count;
    conn_t * closed_head;
    int is_accepting;
    int is_shutting_down;
    long long shutdown_deadline_ms;
    long long now_ms;
} server_t;

typedef struct {
    int port;
    const char * root_path;
    size_t max_connections;
    proxy_config_t * proxy_config;
} server_options_t;

server_t * create_server (const server_options_t * options);
int run_server (server_t * server);
void destroy_server (server_t * server);

/* 아래는 proxy.c가 함께 쓰는 연결 관리 동작이다 */
int register_connection (server_t * server, conn_t * conn);
void close_connection (server_t * server, conn_t * conn);
void set_connection_deadline (server_t * server, conn_t * conn,
    deadline_kind_t kind, long long timeout_ms);
void clear_connection_deadline (server_t * server, conn_t * conn);
void update_interest (server_t * server, conn_t * conn);
void queue_error_response (server_t * server, conn_t * client, int status_code,
    const char * extra_headers);
void finish_response (server_t * server, conn_t * client);
int set_nonblocking (int fd);

#endif
