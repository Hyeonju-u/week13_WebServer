#ifndef CONNECTION_H
#define CONNECTION_H

#include <arpa/inet.h>
#include <stddef.h>
#include <sys/types.h>

#include "buffer.h"
#include "proxy_config.h"

typedef enum {
    CONN_CLIENT,
    CONN_BACKEND
} conn_kind_t;

typedef enum {
    STATE_READING,
    STATE_WRITING,
    STATE_PROXYING,
    STATE_BACKEND_CONNECTING,
    STATE_BACKEND_SENDING,
    STATE_BACKEND_HEADER,
    STATE_BACKEND_BODY,
    STATE_CLOSED
} conn_state_t;

typedef enum {
    DEADLINE_NONE,
    DEADLINE_IDLE,
    DEADLINE_REQUEST,
    DEADLINE_BACKEND_CONNECT,
    DEADLINE_BACKEND_RESPONSE,
    DEADLINE_SHUTDOWN
} deadline_kind_t;

/* 손님과 백엔드가 같은 구조체를 쓰고, 종류별로 쓰는 필드만 다르다 */
typedef struct conn {
    int fd;
    conn_kind_t kind;
    conn_state_t state;
    int is_closed;

    byte_buffer_t input;
    byte_buffer_t output;
    int wants_read;
    int wants_write;

    long long deadline_ms;
    deadline_kind_t deadline_kind;
    long heap_index;

    struct conn * all_previous;
    struct conn * all_next;
    struct conn * pending_previous;
    struct conn * pending_next;
    struct conn * closed_next;
    int is_pending;

    struct conn * peer;

    char client_address[INET6_ADDRSTRLEN];
    int keep_alive;
    int is_head;
    int is_http10;
    int is_half_closed;
    int has_sent_continue;
    int is_response_done;
    int has_response_started;
    int file_fd;
    off_t file_remaining;
    byte_buffer_t forward;
    route_t * route;
    size_t first_backend_index;
    size_t attempt_count;
    int has_timed_out;

    size_t forward_offset;
    int is_body_until_close;
    size_t body_remaining;
} conn_t;

conn_t * create_connection (int fd, conn_kind_t kind);
void destroy_connection (conn_t * conn);

#endif
