#include "proxy.h"

#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "log.h"
#include "proxy_message.h"
#include "settings.h"

#define READ_CHUNK_BYTES 32768

static void try_next_backend (server_t * server, conn_t * client);
static int connect_to_backend (server_t * server, conn_t * client,
    const backend_t * target);
static int attach_backend (server_t * server, conn_t * client, int fd);
static void finish_backend_connect (server_t * server, conn_t * backend);
static void fail_backend_attempt (server_t * server, conn_t * backend,
    int has_timed_out);
static void detach_peers (conn_t * conn);
static long long get_response_timeout_ms (const server_t * server);
static void send_to_backend (server_t * server, conn_t * backend);
static void fail_backend_response (server_t * server, conn_t * backend,
    int status_code);
static void finish_relay (server_t * server, conn_t * backend);
static void begin_backend_response (server_t * server, conn_t * backend);
static void read_from_backend (server_t * server, conn_t * backend);
static void handle_header_bytes (server_t * server, conn_t * backend,
    const char * bytes, size_t length);
static void process_response_head (server_t * server, conn_t * backend);
static void begin_relay (server_t * server, conn_t * backend,
    const backend_response_t * response);
static int decide_body_framing (conn_t * backend, conn_t * client,
    const backend_response_t * response);
static void relay_leftover (server_t * server, conn_t * backend);
static void relay_body_bytes (server_t * server, conn_t * backend,
    const char * bytes, size_t length);
static void handle_body_bytes (server_t * server, conn_t * backend,
    const char * bytes, size_t length);

/* ── 공개 함수 ── */

/*
 * 손님 요청을 백엔드에 보낼 모양으로 한 번 만들어 두고 연결을 시도한다.
 * 연결에 실패하면 같은 바이트를 다음 백엔드에 다시 쓸 수 있다.
 */
void start_proxy_request (server_t * server, conn_t * client,
    const http_request_t * request, route_t * route) {
    if (build_forward_request(&client->forward, request, client->input.data,
            client->client_address) != 0) {
        client->keep_alive = 0;
        queue_error_response(server, client, 503, NULL);
        return;
    }
    client->route = route;
    client->first_backend_index = route->next_index % route->backend_count;
    route->next_index++;
    client->attempt_count = 0;
    client->has_timed_out = 0;
    client->state = STATE_PROXYING;
    clear_connection_deadline(server, client);
    try_next_backend(server, client);
}

void handle_backend_event (server_t * server, conn_t * backend,
    const event_t * event) {
    if (backend->state == STATE_BACKEND_CONNECTING) {
        if (event->writable || event->failed) {
            finish_backend_connect(server, backend);
        }
        return;
    }
    if (backend->state == STATE_BACKEND_SENDING) {
        if (event->writable || event->failed) {
            send_to_backend(server, backend);
        }
        return;
    }
    if (event->readable || event->failed) {
        read_from_backend(server, backend);
    }
}

void handle_backend_timeout (server_t * server, conn_t * backend,
    deadline_kind_t kind) {
    if (kind == DEADLINE_BACKEND_CONNECT) {
        write_log(LOG_INFO, "백엔드 연결 시간 초과");
        fail_backend_attempt(server, backend, 1);
        return;
    }
    write_log(LOG_INFO, "백엔드 응답 시간 초과");
    fail_backend_response(server, backend, 504);
}

/*
 * 손님 출력 버퍼가 64KB를 넘으면 백엔드 읽기를 멈춰서 버퍼가 끝없이 커지는
 * 것을 막는다. 줄어들면 손님 쪽 쓰기가 이 함수로 다시 켠다 (PRX-15).
 */
void compute_backend_interest (const conn_t * backend, int * wants_read,
    int * wants_write) {
    *wants_read = 0;
    *wants_write = 0;
    if (backend->state == STATE_BACKEND_CONNECTING
        || backend->state == STATE_BACKEND_SENDING) {
        *wants_write = 1;
    } else if (backend->state == STATE_BACKEND_HEADER) {
        *wants_read = 1;
    } else if (backend->state == STATE_BACKEND_BODY && backend->peer != NULL) {
        *wants_read = backend->peer->output.length <= BACKPRESSURE_BYTES;
    }
}

/* ── 도우미 함수 ── */

static void try_next_backend (server_t * server, conn_t * client) {
    const route_t * route = client->route;
    while (client->attempt_count < route->backend_count) {
        size_t index = (client->first_backend_index + client->attempt_count)
            % route->backend_count;
        client->attempt_count++;
        if (connect_to_backend(server, client, &route->backends[index]) == 0) {
            return;
        }
        client->has_timed_out = 0;
    }
    int status_code = client->has_timed_out ? 504 : 502;
    queue_error_response(server, client, status_code, NULL);
}

/* 0이면 연결을 시작했다(결과는 쓰기 알림으로 온다). -1이면 시작도 못 했다 */
static int connect_to_backend (server_t * server, conn_t * client,
    const backend_t * target) {
    int fd = socket(target->address.ss_family, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    if (set_nonblocking(fd) != 0
        || (connect(fd, (const struct sockaddr *) &target->address,
                target->address_length) != 0 && errno != EINPROGRESS)) {
        close(fd);
        return -1;
    }
    return attach_backend(server, client, fd);
}

/* fd의 주인은 성공하면 백엔드 conn이고, 실패하면 이 함수가 닫는다 */
static int attach_backend (server_t * server, conn_t * client, int fd) {
    conn_t * backend = create_connection(fd, CONN_BACKEND);
    if (backend == NULL) {
        close(fd);
        return -1;
    }
    backend->state = STATE_BACKEND_CONNECTING;
    backend->wants_write = 1;
    if (register_connection(server, backend) != 0) {
        destroy_connection(backend);
        return -1;
    }
    backend->peer = client;
    client->peer = backend;
    set_connection_deadline(server, backend, DEADLINE_BACKEND_CONNECT,
        (long long) server->proxy_config->connect_timeout_seconds * 1000);
    return 0;
}

/* connect는 non-blocking이라 성공 여부를 SO_ERROR로 확인해야 한다 (PRX-08) */
static void finish_backend_connect (server_t * server, conn_t * backend) {
    int error_number = 0;
    socklen_t length = sizeof(error_number);
    if (getsockopt(backend->fd, SOL_SOCKET, SO_ERROR, &error_number,
            &length) != 0) {
        error_number = errno;
    }
    if (error_number != 0) {
        write_log(LOG_DEBUG, "백엔드 연결 실패: %s", strerror(error_number));
        fail_backend_attempt(server, backend, 0);
        return;
    }
    backend->state = STATE_BACKEND_SENDING;
    set_connection_deadline(server, backend, DEADLINE_BACKEND_RESPONSE,
        get_response_timeout_ms(server));
    update_interest(server, backend);
}

/*
 * 요청을 한 바이트도 보내기 전인 연결 단계에서만 다음 백엔드로 다시
 * 시도한다. 보낸 뒤에 다시 보내면 POST가 두 번 실행될 수 있다 (PRX-09).
 */
static void fail_backend_attempt (server_t * server, conn_t * backend,
    int has_timed_out) {
    conn_t * client = backend->peer;
    client->has_timed_out = has_timed_out;
    detach_peers(backend);
    close_connection(server, backend);
    try_next_backend(server, client);
}

static void detach_peers (conn_t * conn) {
    if (conn->peer != NULL) {
        conn->peer->peer = NULL;
        conn->peer = NULL;
    }
}

static long long get_response_timeout_ms (const server_t * server) {
    return (long long) server->proxy_config->response_timeout_seconds * 1000;
}

static void send_to_backend (server_t * server, conn_t * backend) {
    conn_t * client = backend->peer;
    const byte_buffer_t * forward = &client->forward;
    ssize_t sent = write(backend->fd, forward->data + backend->forward_offset,
        forward->length - backend->forward_offset);
    if (sent < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            fail_backend_response(server, backend, 502);
        }
        return;
    }
    backend->forward_offset += (size_t) sent;
    if (backend->forward_offset < forward->length) {
        return;
    }
    begin_backend_response(server, backend);
}

/*
 * 이미 응답을 보내기 시작했다면 상태 코드를 바꿀 수 없다. 모아 둔 바이트를
 * 마저 보낸 뒤 연결을 닫아서, 손님이 응답이 중간에 끊겼음을 알게 한다.
 */
static void fail_backend_response (server_t * server, conn_t * backend,
    int status_code) {
    conn_t * client = backend->peer;
    if (client->has_response_started) {
        client->keep_alive = 0;
        finish_relay(server, backend);
        return;
    }
    detach_peers(backend);
    close_connection(server, backend);
    queue_error_response(server, client, status_code, NULL);
}

/* 응답이 끝났다. 백엔드는 닫고 손님 연결은 keep-alive 규칙대로 이어 간다 */
static void finish_relay (server_t * server, conn_t * backend) {
    conn_t * client = backend->peer;
    detach_peers(backend);
    close_connection(server, backend);
    client->is_response_done = 1;
    if (client->output.length == 0) {
        finish_response(server, client);
        return;
    }
    update_interest(server, client);
}

/* 요청을 다 보냈다. 이제 재시도는 없으니 보관하던 요청 바이트를 버린다 */
static void begin_backend_response (server_t * server, conn_t * backend) {
    release_buffer(&backend->peer->forward);
    backend->state = STATE_BACKEND_HEADER;
    set_connection_deadline(server, backend, DEADLINE_BACKEND_RESPONSE,
        get_response_timeout_ms(server));
    update_interest(server, backend);
}

static void read_from_backend (server_t * server, conn_t * backend) {
    conn_t * client = backend->peer;
    if (client->output.length > BACKPRESSURE_BYTES) {
        update_interest(server, backend);
        return;
    }
    char chunk[READ_CHUNK_BYTES];
    ssize_t received = read(backend->fd, chunk, sizeof(chunk));
    if (received < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            fail_backend_response(server, backend, 502);
        }
        return;
    }
    if (received > 0) {
        set_connection_deadline(server, backend, DEADLINE_BACKEND_RESPONSE,
            get_response_timeout_ms(server));
    }
    if (backend->state == STATE_BACKEND_HEADER) {
        handle_header_bytes(server, backend, chunk, (size_t) received);
        return;
    }
    handle_body_bytes(server, backend, chunk, (size_t) received);
}

/* length가 0이면 백엔드가 헤더를 다 보내기 전에 끊은 것이다 */
static void handle_header_bytes (server_t * server, conn_t * backend,
    const char * bytes, size_t length) {
    if (length == 0) {
        fail_backend_response(server, backend, 502);
        return;
    }
    if (append_bytes(&backend->input, bytes, length) != 0) {
        fail_backend_response(server, backend, 503);
        return;
    }
    process_response_head(server, backend);
}

static void process_response_head (server_t * server, conn_t * backend) {
    for (;;) {
        backend_response_t response;
        parse_result_t result = parse_backend_response(backend->input.data,
            backend->input.length, &response);
        if (result == PARSE_NEED_MORE) {
            return;
        }
        if (result == PARSE_ERROR || response.status_code == 101) {
            fail_backend_response(server, backend, 502);
            return;
        }
        if (response.status_code >= 200) {
            begin_relay(server, backend, &response);
            return;
        }
        consume_bytes(&backend->input, response.head_length);
    }
}

static void begin_relay (server_t * server, conn_t * backend,
    const backend_response_t * response) {
    conn_t * client = backend->peer;
    if (decide_body_framing(backend, client, response) != 0) {
        fail_backend_response(server, backend, 502);
        return;
    }
    if (append_relayed_head(&client->output, backend->input.data, response,
            client->keep_alive) != 0) {
        fail_backend_response(server, backend, 503);
        return;
    }
    client->has_response_started = 1;
    backend->state = STATE_BACKEND_BODY;
    consume_bytes(&backend->input, response->head_length);
    relay_leftover(server, backend);
}

/*
 * 본문이 어디서 끝나는지 정한다 (PRX-12). 길이를 알 수 없으면 백엔드가 닫을
 * 때까지 넘기고 손님 연결도 닫는다.
 */
static int decide_body_framing (conn_t * backend, conn_t * client,
    const backend_response_t * response) {
    int status_code = response->status_code;
    if (client->is_head || status_code == 204 || status_code == 304) {
        backend->is_body_until_close = 0;
        backend->body_remaining = 0;
        return 0;
    }
    if (response->has_transfer_encoding && client->is_http10) {
        return -1;
    }
    if (response->has_content_length) {
        backend->is_body_until_close = 0;
        backend->body_remaining = response->content_length;
        return 0;
    }
    backend->is_body_until_close = 1;
    client->keep_alive = 0;
    return 0;
}

/* 헤더와 같이 도착한 본문 조각을 먼저 넘긴다 */
static void relay_leftover (server_t * server, conn_t * backend) {
    if (!backend->is_body_until_close && backend->body_remaining == 0) {
        finish_relay(server, backend);
        return;
    }
    if (backend->input.length > 0) {
        relay_body_bytes(server, backend, backend->input.data,
            backend->input.length);
    }
    if (backend->is_closed) {
        return;
    }
    release_buffer(&backend->input);
    update_interest(server, backend->peer);
    update_interest(server, backend);
}

static void relay_body_bytes (server_t * server, conn_t * backend,
    const char * bytes, size_t length) {
    conn_t * client = backend->peer;
    size_t usable = length;
    if (!backend->is_body_until_close && usable > backend->body_remaining) {
        usable = backend->body_remaining;
    }
    if (append_bytes(&client->output, bytes, usable) != 0) {
        close_connection(server, client);
        return;
    }
    if (!backend->is_body_until_close) {
        backend->body_remaining -= usable;
        if (backend->body_remaining == 0) {
            finish_relay(server, backend);
            return;
        }
    }
    update_interest(server, client);
    update_interest(server, backend);
}

/* length가 0이면 백엔드가 연결을 닫은 것이다 */
static void handle_body_bytes (server_t * server, conn_t * backend,
    const char * bytes, size_t length) {
    if (length > 0) {
        relay_body_bytes(server, backend, bytes, length);
        return;
    }
    if (backend->is_body_until_close || backend->body_remaining == 0) {
        finish_relay(server, backend);
        return;
    }
    write_log(LOG_INFO, "백엔드가 본문을 보내다 끊김");
    fail_backend_response(server, backend, 502);
}
