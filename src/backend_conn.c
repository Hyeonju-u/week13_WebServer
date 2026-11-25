#include "backend_conn.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "proxy_request.h"
#include "proxy_response.h"
#include "upstream.h"

/* (G12, G13, G36, G39) 요청을 보낸 뒤의 실패 하나. 재시도는 하지 않는다 */
typedef struct {
    int status;
    /* (G39) 로그 원인. NULL = 우리 쪽 문제 (500) */
    const char * cause;
    /* (G18) stderr에 찍는 설명 */
    const char * what;
    /* (G13, G41) connect 실패·타임아웃·도중 끊김만 뺀다 */
    bool should_mark;
} backend_failure_t;

static const backend_failure_t SEND_TIMEOUT = {
    504, "timeout", "send timeout", true };
static const backend_failure_t READ_TIMEOUT = {
    504, "timeout", "read timeout", true };
static const backend_failure_t CONNECTION_BROKEN = {
    502, "reset", "connection broken", true };
static const backend_failure_t INVALID_RESPONSE = {
    502, "invalid", "invalid response", false };
static const backend_failure_t CHUNKED_RESPONSE = {
    502, "chunked", "chunked response", false };
static const backend_failure_t RESPONSE_TOO_LARGE = {
    502, "too_large", "response too large", false };
static const backend_failure_t OUT_OF_MEMORY = {
    500, NULL, "out of memory", false };

static char * copy_bytes (const char * source, size_t length);
static void try_next_backend (loop_t * loop, conn_t ** slot);
static void open_backend (loop_t * loop, conn_t ** slot);
static int create_backend_socket (void);
static void report_backend_error (const loop_t * loop, const conn_t * conn,
                                  const char * what, const char * detail);
static void register_backend (loop_t * loop, conn_t ** slot,
                              conn_state_t state, timer_list_t * timers);
static void fail_connect (loop_t * loop, conn_t ** slot, int error);
static void check_connect (loop_t * loop, conn_t ** slot);
static void enter_backend_state (loop_t * loop, conn_t ** slot,
                                 conn_state_t state, uint32_t events,
                                 timer_list_t * timers);
static void fail_backend (loop_t * loop, conn_t ** slot,
                          const backend_failure_t * failure);
static void send_backend_part (loop_t * loop, conn_t ** slot);
static size_t pick_request_part (const conn_t * conn, const char ** data);
static bool is_retry_error (int error);
static bool is_request_sent (const conn_t * conn);
static void read_backend_part (loop_t * loop, conn_t ** slot);
static bool reserve_response_space (const loop_t * loop, proxy_t * proxy);
static size_t find_capacity_limit (const loop_t * loop,
                                   const proxy_t * proxy);
static bool resize_response (proxy_t * proxy, size_t capacity);
static void finish_at_close (loop_t * loop, conn_t ** slot);
static void finish_backend_response (loop_t * loop, conn_t ** slot);
static size_t find_received_body (const proxy_t * proxy);
static void process_response (loop_t * loop, conn_t ** slot);
static backend_head_result find_response_head (const loop_t * loop,
                                               proxy_t * proxy);
static const backend_failure_t * pick_head_failure (
    backend_head_result result);
static bool reserve_known_length (proxy_t * proxy);
static bool is_body_too_large (const loop_t * loop, const proxy_t * proxy);
static bool is_body_complete (const proxy_t * proxy);

/* ── 공개 함수 ── */

/* (G6, G28) 할당을 다 끝낸 뒤에 proxy를 채운다 */
int create_proxy (loop_t * loop, conn_t * conn, const route_t * route) {
    proxy_t * proxy = &conn->proxy;
    size_t length = write_backend_request(&conn->request, &conn->peer,
                                          loop->head);

    if (length == 0) {
        return 431;
    }
    char * request_head = copy_bytes(loop->head, length);
    bool * tried = calloc(route->backend_count, sizeof(*tried));
    if (request_head == NULL || tried == NULL) {
        free(request_head);
        free(tried);
        return 500;
    }
    proxy->route = route;
    proxy->tried = tried;
    proxy->attempts = 0;
    proxy->request_head = request_head;
    proxy->request_head_length = length;
    proxy->request_head_sent = 0;
    proxy->body_sent = 0;
    proxy->failure_status = 502;
    proxy->cause = NULL;
    return 200;
}

/* (G26) 종류 표시·주인·fd는 카드가 사는 동안 그대로 둔다 */
void destroy_proxy (proxy_t * proxy) {
    free(proxy->tried);
    free(proxy->request_head);
    proxy->tried = NULL;
    proxy->request_head = NULL;
    proxy->route = NULL;
    proxy->attempts = 0;
    proxy->request_head_length = 0;
    proxy->request_head_sent = 0;
    proxy->body_sent = 0;
    proxy->started_ms = 0;
    proxy->finished_ms = 0;
    proxy->failure_status = 0;
    proxy->cause = NULL;
}

/* (G11, G21) 손님 fd는 FIN·HUP만 본다 (EPOLLIN을 보면 미리 보낸 다음 요청에
 * level-triggered로 계속 깨어남, L3) */
void start_backend (loop_t * loop, conn_t ** slot) {
    conn_t * conn = *slot;

    if (!watch_client(loop, conn, EPOLLRDHUP)) {
        destroy_conn(loop, slot, "close");
        return;
    }
    conn->proxy.started_ms = read_clock_ms();
    try_next_backend(loop, slot);
}

/* (G8, G11, G30) 이미 백엔드를 놓은 상태면 같은 묶음의 지난 이벤트라 무시 */
void handle_backend_event (loop_t * loop, conn_t ** slot, uint32_t events) {
    (void) events;
    switch ((*slot)->state) {
    case CONN_CONNECT:
        check_connect(loop, slot);
        return;
    case CONN_BACKEND_SEND:
        send_backend_part(loop, slot);
        return;
    case CONN_BACKEND_READ:
        read_backend_part(loop, slot);
        return;
    default:
        return;
    }
}

/* (G11, G12, G13) connect 타임아웃만 다음 백엔드로 넘어간다 */
void expire_backend (loop_t * loop, conn_t ** slot) {
    switch ((*slot)->state) {
    case CONN_CONNECT:
        fail_connect(loop, slot, ETIMEDOUT);
        return;
    case CONN_BACKEND_SEND:
        fail_backend(loop, slot, &SEND_TIMEOUT);
        return;
    default:
        fail_backend(loop, slot, &READ_TIMEOUT);
        return;
    }
}

/* (G14, G26) close가 epoll에서도 뺀다. 넘겨준 응답 버퍼는 이미 NULL (G47) */
void release_backend (loop_t * loop, conn_t * conn) {
    proxy_t * proxy = &conn->proxy;

    if (proxy->fd < 0) {
        return;
    }
    close(proxy->fd);
    proxy->fd = -1;
    remove_backend_use(loop->upstream, proxy->route, proxy->position);
    free(proxy->response);
    proxy->response = NULL;
    proxy->response_length = 0;
    proxy->response_capacity = 0;
    proxy->has_head = false;
    proxy->request_head_sent = 0;
    proxy->body_sent = 0;
    proxy->finished_ms = read_clock_ms();
}

/* ── 도우미 함수 ── */

/* (G28) 반환: NULL = 할당 실패. 돌려받은 쪽이 free */
static char * copy_bytes (const char * source, size_t length) {
    char * copy = malloc(length > 0 ? length : 1);
    if (copy == NULL) {
        return NULL;
    }
    memcpy(copy, source, length);
    return copy;
}

/* (G12, G13, G22, G36) 더 해 볼 백엔드가 없으면 마지막 실패로 답한다 */
static void try_next_backend (loop_t * loop, conn_t ** slot) {
    proxy_t * proxy = &(*slot)->proxy;
    size_t position = 0;

    if (!pick_backend(loop->upstream, proxy->route, proxy->tried,
                      read_clock_ms(), &position)) {
        respond_with_proxy_status(loop, slot, proxy->failure_status);
        return;
    }
    proxy->tried[position] = true;
    proxy->attempts++;
    proxy->position = position;
    open_backend(loop, slot);
}

/* (G11, G14, G30, G36) 소켓 → +1 → non-blocking connect.
 * 바로 끝나면 요청 쓰기, EINPROGRESS면 EPOLLOUT을 기다린다 */
static void open_backend (loop_t * loop, conn_t ** slot) {
    conn_t * conn = *slot;
    proxy_t * proxy = &conn->proxy;

    int fd = create_backend_socket();
    if (fd < 0) {
        report_backend_error(loop, conn, "socket", strerror(errno));
        respond_with_proxy_status(loop, slot, 500);
        return;
    }
    proxy->fd = fd;
    add_backend_use(loop->upstream, proxy->route, proxy->position);
    const struct sockaddr_in * address = find_backend_address(
        loop->upstream, proxy->route, proxy->position);
    if (connect(fd, (const struct sockaddr *) address,
                sizeof(*address)) == 0) {
        mark_backend_alive(loop->upstream, proxy->route, proxy->position);
        register_backend(loop, slot, CONN_BACKEND_SEND,
                         &loop->backend_send_timers);
        return;
    }
    if (errno != EINPROGRESS) {
        fail_connect(loop, slot, errno);
        return;
    }
    register_backend(loop, slot, CONN_CONNECT, &loop->connect_timers);
}

/* (G30, L31) 머리 쓰기 뒤 작은 본문 쓰기가 Nagle + 지연 ACK로 멈추지 않게.
 * 반환: -1 = 실패 (errno 유지) */
static int create_backend_socket (void) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    int enabled = 1;

    if (fd < 0) {
        return -1;
    }
    int flags = fcntl(fd, F_GETFL);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0
        || setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &enabled,
                      sizeof(enabled)) != 0) {
        int error = errno;
        close(fd);
        errno = error;
        return -1;
    }
    return fd;
}

/* (G18) "backend 127.0.0.1:8001: connect: Connection refused" */
static void report_backend_error (const loop_t * loop, const conn_t * conn,
                                  const char * what, const char * detail) {
    char address[INET_ADDRSTRLEN];
    const struct sockaddr_in * backend = find_backend_address(
        loop->upstream, conn->proxy.route, conn->proxy.position);

    if (inet_ntop(AF_INET, &backend->sin_addr, address,
                  sizeof(address)) == NULL) {
        snprintf(address, sizeof(address), "-");
    }
    fprintf(stderr, "backend %s:%u: %s%s%s\n", address,
            (unsigned) ntohs(backend->sin_port), what,
            detail == NULL ? "" : ": ", detail == NULL ? "" : detail);
}

/* (G25, G30) 백엔드 fd는 &conn->proxy로 등록해 손님 fd와 구분한다 */
static void register_backend (loop_t * loop, conn_t ** slot,
                              conn_state_t state, timer_list_t * timers) {
    conn_t * conn = *slot;
    struct epoll_event event = { .events = EPOLLOUT,
                                 .data.ptr = &conn->proxy };

    if (epoll_ctl(loop->epoll_fd, EPOLL_CTL_ADD, conn->proxy.fd,
                  &event) != 0) {
        report_backend_error(loop, conn, "epoll_ctl", strerror(errno));
        fail_backend(loop, slot, &OUT_OF_MEMORY);
        return;
    }
    conn->state = state;
    start_timer(timers, &conn->timer, read_clock_ms());
}

/* (G12, G13, G36) connect 단계 실패: 빼고 → 놓고 → 다음 백엔드로.
 * 응답 코드는 마지막 실패로 정한다 (거절 502, 타임아웃 504) */
static void fail_connect (loop_t * loop, conn_t ** slot, int error) {
    conn_t * conn = *slot;
    proxy_t * proxy = &conn->proxy;
    bool is_timeout = error == ETIMEDOUT;

    report_backend_error(loop, conn, "connect",
                         is_timeout ? "timeout" : strerror(error));
    mark_backend_failed(loop->upstream, proxy->route, proxy->position,
                        read_clock_ms());
    proxy->failure_status = is_timeout ? 504 : 502;
    proxy->cause = is_timeout ? "timeout" : "refused";
    release_backend(loop, conn);
    try_next_backend(loop, slot);
}

/* (G30) EPOLLOUT 뒤 SO_ERROR로 결과를 본다 */
static void check_connect (loop_t * loop, conn_t ** slot) {
    conn_t * conn = *slot;
    int error = 0;
    socklen_t length = sizeof(error);

    if (getsockopt(conn->proxy.fd, SOL_SOCKET, SO_ERROR, &error,
                   &length) != 0) {
        error = errno;
    }
    if (error != 0) {
        fail_connect(loop, slot, error);
        return;
    }
    mark_backend_alive(loop->upstream, conn->proxy.route,
                       conn->proxy.position);
    enter_backend_state(loop, slot, CONN_BACKEND_SEND, EPOLLOUT,
                        &loop->backend_send_timers);
}

/* (L3, G30) 백엔드 fd의 관심 이벤트와 타이머를 함께 바꾼다 */
static void enter_backend_state (loop_t * loop, conn_t ** slot,
                                 conn_state_t state, uint32_t events,
                                 timer_list_t * timers) {
    conn_t * conn = *slot;
    struct epoll_event event = { .events = events,
                                 .data.ptr = &conn->proxy };

    if (epoll_ctl(loop->epoll_fd, EPOLL_CTL_MOD, conn->proxy.fd,
                  &event) != 0) {
        report_backend_error(loop, conn, "epoll_ctl", strerror(errno));
        fail_backend(loop, slot, &OUT_OF_MEMORY);
        return;
    }
    conn->state = state;
    start_timer(timers, &conn->timer, read_clock_ms());
}

/* (G12, G13, G18) 보낸 뒤의 실패: (빼고) → 놓고 → 우리가 만든 응답 */
static void fail_backend (loop_t * loop, conn_t ** slot,
                          const backend_failure_t * failure) {
    conn_t * conn = *slot;
    proxy_t * proxy = &conn->proxy;

    report_backend_error(loop, conn, failure->what, NULL);
    if (failure->should_mark) {
        mark_backend_failed(loop->upstream, proxy->route, proxy->position,
                            read_clock_ms());
    }
    proxy->cause = failure->cause;
    release_backend(loop, conn);
    respond_with_proxy_status(loop, slot, failure->status);
}

/* (G5, G28, L30) 한 번 깨어날 때 쓰기 1번: 헤더 남은 것, 아니면 본문 8KB.
 * 쓰기 타임아웃은 "사이" (G11) */
static void send_backend_part (loop_t * loop, conn_t ** slot) {
    conn_t * conn = *slot;
    proxy_t * proxy = &conn->proxy;
    const char * data = NULL;
    size_t wanted = pick_request_part(conn, &data);

    ssize_t written = write(proxy->fd, data, wanted);
    if (written < 0) {
        if (is_retry_error(errno)) {
            return;
        }
        fail_backend(loop, slot, &CONNECTION_BROKEN);
        return;
    }
    if (proxy->request_head_sent < proxy->request_head_length) {
        proxy->request_head_sent += (size_t) written;
    } else {
        proxy->body_sent += (size_t) written;
    }
    if (is_request_sent(conn)) {
        enter_backend_state(loop, slot, CONN_BACKEND_READ, EPOLLIN,
                            &loop->backend_read_timers);
        return;
    }
    start_timer(&loop->backend_send_timers, &conn->timer, read_clock_ms());
}

/* (G28, G33) 반환: 이번에 쓸 길이. *data: 쓸 자리 */
static size_t pick_request_part (const conn_t * conn, const char ** data) {
    const proxy_t * proxy = &conn->proxy;
    size_t remaining = 0;

    if (proxy->request_head_sent < proxy->request_head_length) {
        *data = proxy->request_head + proxy->request_head_sent;
        return proxy->request_head_length - proxy->request_head_sent;
    }
    *data = conn->request.body + proxy->body_sent;
    remaining = conn->request.content_length - proxy->body_sent;
    return remaining < FILE_CHUNK_SIZE ? remaining : FILE_CHUNK_SIZE;
}

/* (R3c) */
static bool is_retry_error (int error) {
    return error == EAGAIN || error == EWOULDBLOCK || error == EINTR;
}

static bool is_request_sent (const conn_t * conn) {
    return conn->proxy.request_head_sent == conn->proxy.request_head_length
           && conn->proxy.body_sent == conn->request.content_length;
}

/* (G8, G11, G27, L30) 한 번 깨어날 때 읽기 1번, 버퍼 남은 자리만큼.
 * 읽기 타임아웃은 "사이"라 읽을 때마다 다시 건다 */
static void read_backend_part (loop_t * loop, conn_t ** slot) {
    conn_t * conn = *slot;
    proxy_t * proxy = &conn->proxy;

    if (!reserve_response_space(loop, proxy)) {
        fail_backend(loop, slot, &OUT_OF_MEMORY);
        return;
    }
    ssize_t count = read(proxy->fd, proxy->response + proxy->response_length,
                         proxy->response_capacity - proxy->response_length);
    if (count < 0) {
        if (is_retry_error(errno)) {
            return;
        }
        fail_backend(loop, slot, &CONNECTION_BROKEN);
        return;
    }
    if (count == 0) {
        finish_at_close(loop, slot);
        return;
    }
    proxy->response_length += (size_t) count;
    start_timer(&loop->backend_read_timers, &conn->timer, read_clock_ms());
    process_response(loop, slot);
}

/* (G27) 처음엔 8KB(헤더가 다 들어가는 크기), 차면 두 배씩 상한까지.
 * 상한 = 헤더 + 본문 상한 + 1 (넘는지 알 수 있게) */
static bool reserve_response_space (const loop_t * loop, proxy_t * proxy) {
    if (proxy->response == NULL) {
        return resize_response(proxy, MAX_HEADER_SIZE);
    }
    if (proxy->response_length < proxy->response_capacity) {
        return true;
    }
    size_t limit = find_capacity_limit(loop, proxy);
    size_t doubled = proxy->response_capacity * 2;
    return resize_response(proxy, doubled < limit ? doubled : limit);
}

/* (G9, G27) 헤더를 아직 못 찾았으면 헤더 상한 8KB로 본다 */
static size_t find_capacity_limit (const loop_t * loop,
                                   const proxy_t * proxy) {
    size_t head_length = proxy->has_head ? proxy->head.head_length
                                         : MAX_HEADER_SIZE;

    return head_length + loop->config->max_backend_response + 1;
}

/* (G27) 할당이 성공한 뒤에만 바꾼다 */
static bool resize_response (proxy_t * proxy, size_t capacity) {
    char * grown = realloc(proxy->response, capacity);

    if (grown == NULL) {
        return false;
    }
    proxy->response = grown;
    proxy->response_capacity = capacity;
    return true;
}

/* (G8) 백엔드가 닫음: 길이 없는 응답은 여기서 끝(1.0 방식),
 * 헤더 전이거나 Content-Length보다 덜 왔으면 도중 끊김 */
static void finish_at_close (loop_t * loop, conn_t ** slot) {
    proxy_t * proxy = &(*slot)->proxy;

    if (!proxy->has_head
        || (proxy->head.has_body && proxy->head.has_content_length)) {
        fail_backend(loop, slot, &CONNECTION_BROKEN);
        return;
    }
    finish_backend_response(loop, slot);
}

/* (G8, G10, G39, G47) 응답 헤더를 고치고, 응답 버퍼를 손님 응답 본문으로
 * 넘긴 뒤 백엔드를 놓는다 (넘는 바이트는 Content-Length로 잘림).
 * 성공한 줄에는 앞 시도의 실패 원인을 남기지 않는다 (retry=N만) */
static void finish_backend_response (loop_t * loop, conn_t ** slot) {
    conn_t * conn = *slot;
    proxy_t * proxy = &conn->proxy;
    size_t body_length = find_received_body(proxy);

    if (proxy->head.has_content_length) {
        body_length = proxy->head.content_length;
    }
    if (!proxy->head.has_body) {
        body_length = 0;
    }
    size_t head_length = write_client_head(proxy->response, &proxy->head,
                                           body_length,
                                           conn->request.keep_alive,
                                           loop->head);
    char * body = proxy->response;
    size_t body_start = proxy->head.head_length;
    int status = proxy->head.status;
    proxy->response = NULL;
    proxy->cause = NULL;
    release_backend(loop, conn);
    start_proxy_response(loop, slot, head_length, body, body_start,
                         body_length, status);
}

/* (G8) 헤더 뒤에 받은 바이트 수 */
static size_t find_received_body (const proxy_t * proxy) {
    return proxy->response_length - proxy->head.head_length;
}

/* (G8, G9, G27) 헤더 → 길이 맞추기 → 상한 → 끝났는지 */
static void process_response (loop_t * loop, conn_t ** slot) {
    proxy_t * proxy = &(*slot)->proxy;

    backend_head_result result = find_response_head(loop, proxy);
    if (result == BACKEND_HEAD_NEED_MORE) {
        return;
    }
    if (result != BACKEND_HEAD_OK) {
        fail_backend(loop, slot, pick_head_failure(result));
        return;
    }
    if (!reserve_known_length(proxy)) {
        fail_backend(loop, slot, &OUT_OF_MEMORY);
        return;
    }
    if (is_body_too_large(loop, proxy)) {
        fail_backend(loop, slot, &RESPONSE_TOO_LARGE);
        return;
    }
    if (is_body_complete(proxy)) {
        finish_backend_response(loop, slot);
    }
}

/* (G8) 1xx는 그 응답만 버퍼에서 지우고 다음 응답을 본다 */
static backend_head_result find_response_head (const loop_t * loop,
                                               proxy_t * proxy) {
    backend_head_result result = BACKEND_HEAD_OK;

    while (!proxy->has_head) {
        result = parse_backend_head(proxy->response, proxy->response_length,
                                    loop->config->max_backend_response,
                                    &proxy->head);
        if (result != BACKEND_HEAD_INTERIM) {
            proxy->has_head = result == BACKEND_HEAD_OK;
            return result;
        }
        remove_buffer_prefix(proxy->response, &proxy->response_length,
                             proxy->head.head_length);
    }
    return result;
}

/* (G8, G9, G12, G39) */
static const backend_failure_t * pick_head_failure (
    backend_head_result result) {
    if (result == BACKEND_HEAD_CHUNKED) {
        return &CHUNKED_RESPONSE;
    }
    if (result == BACKEND_HEAD_TOO_LARGE) {
        return &RESPONSE_TOO_LARGE;
    }
    return &INVALID_RESPONSE;
}

/* (G27) Content-Length를 알면 한 번에 헤더 + 길이로 맞춘다 (줄이지는 않음) */
static bool reserve_known_length (proxy_t * proxy) {
    size_t wanted = proxy->head.head_length + proxy->head.content_length;

    if (!proxy->head.has_body || !proxy->head.has_content_length
        || wanted <= proxy->response_capacity) {
        return true;
    }
    return resize_response(proxy, wanted);
}

/* (G9) 길이 없는 응답이 본문 상한을 넘음 */
static bool is_body_too_large (const loop_t * loop, const proxy_t * proxy) {
    return proxy->head.has_body && !proxy->head.has_content_length
           && find_received_body(proxy) > loop->config->max_backend_response;
}

/* (G8) 204·304는 헤더에서 끝, Content-Length는 그만큼 받으면 끝.
 * 길이 없는 응답은 닫힐 때 끝 (finish_at_close) */
static bool is_body_complete (const proxy_t * proxy) {
    if (!proxy->head.has_body) {
        return true;
    }
    return proxy->head.has_content_length
           && find_received_body(proxy) >= proxy->head.content_length;
}
