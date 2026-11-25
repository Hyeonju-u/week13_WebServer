#include "connection.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "access_log.h"
#include "backend_conn.h"
#include "http_response.h"
#include "router.h"
#include "upstream.h"

/* (G7, G38) 손님에게 보내는 중간 응답. 끝의 '\0'은 보내지 않는다 */
static const char CONTINUE_LINE[] = "HTTP/1.1 100 Continue\r\n\r\n";
#define CONTINUE_LINE_LENGTH (sizeof(CONTINUE_LINE) - 1)

static void read_request_head (loop_t * loop, conn_t ** slot);
static bool is_retry_error (int error);
static void note_received (loop_t * loop, conn_t * conn);
static void process_buffered_request (loop_t * loop, conn_t ** slot);
static void respond_with_error (loop_t * loop, conn_t ** slot, int status);
static void start_response (loop_t * loop, conn_t ** slot,
                            const response_head_t * head);
static void begin_response (loop_t * loop, conn_t ** slot, size_t length,
                            int status, size_t body_length);
static char * copy_bytes (const char * source, size_t length);
static void report_error (const char * what);
static void enter_state (loop_t * loop, conn_t ** slot, conn_state_t state,
                         uint32_t events, timer_list_t * timers);
static void start_request (loop_t * loop, conn_t ** slot,
                           size_t header_length);
static void start_proxy_request (loop_t * loop, conn_t ** slot,
                                 const route_t * route);
static bool has_continue_expect (const request_t * request);
static void receive_body_or_connect (loop_t * loop, conn_t ** slot);
static void start_static_request (loop_t * loop, conn_t ** slot);
static void decide_connection_close (conn_t * conn);
static bool has_expect_header (const request_t * request);
static void send_response (loop_t * loop, conn_t ** slot);
static void send_head_part (loop_t * loop, conn_t ** slot);
static void handle_write_error (loop_t * loop, conn_t ** slot);
static void note_progress (loop_t * loop, conn_t ** slot, size_t written);
static bool is_response_sent (const conn_t * conn);
static void finish_response (loop_t * loop, conn_t ** slot);
static void log_request (loop_t * loop, conn_t * conn, const char * tag);
static size_t find_request_line_length (const request_t * request);
static void add_backend_entry (const loop_t * loop, const conn_t * conn,
                               access_entry_t * entry);
static void release_response (conn_t * conn);
static void start_lingering (loop_t * loop, conn_t ** slot);
static void start_discarding (loop_t * loop, conn_t ** slot);
static void finish_request (loop_t * loop, conn_t ** slot);
static void reset_request (conn_t * conn);
static void send_memory_chunk (loop_t * loop, conn_t ** slot);
static size_t pick_chunk_size (size_t remaining);
static void send_file_chunk (loop_t * loop, conn_t ** slot);
static void discard_request_body (loop_t * loop, conn_t ** slot);
static void keep_reading_quietly (loop_t * loop, conn_t ** slot);
static void read_lingering (loop_t * loop, conn_t ** slot);
static void send_continue (loop_t * loop, conn_t ** slot);
static void read_request_body (loop_t * loop, conn_t ** slot);
static void expire_header (loop_t * loop, conn_t ** slot);
static void expire_body (loop_t * loop, conn_t ** slot);
static void report_card_counts (const loop_t * loop);

/* ── 공개 함수 ── */

/* (L28, G25) 카드가 커서(연결 버퍼 8KB) 복합 리터럴 대신 memset으로 비운다 */
conn_t * create_conn (int fd, const struct sockaddr_in * peer) {
    conn_t * conn = malloc(sizeof(*conn));
    if (conn == NULL) {
        return NULL;
    }
    memset(conn, 0, sizeof(*conn));
    conn->kind = ENDPOINT_CLIENT;
    conn->fd = fd;
    conn->peer = *peer;
    conn->state = CONN_READ_HEAD;
    conn->result.fd = -1;
    conn->timer.owner = conn;
    conn->proxy.kind = ENDPOINT_BACKEND;
    conn->proxy.owner = conn;
    conn->proxy.fd = -1;
    return conn;
}

/* (L1, L5, L24, L26) 새 연결의 헤더 타이머는 accept부터 잰다 */
bool start_conn (loop_t * loop, conn_t * conn) {
    struct epoll_event event = { .events = EPOLLIN, .data.ptr = conn };

    if (epoll_ctl(loop->epoll_fd, EPOLL_CTL_ADD, conn->fd, &event) != 0) {
        return false;
    }
    start_timer(&loop->header_timers, &conn->timer, read_clock_ms());
    return true;
}

/* (L3, L24) 손님 half-close는 따로 보지 않고 read 0으로 안다 (L14, L27).
 * 백엔드를 기다리는 동안은 끊김(FIN·HUP)만 본다 (G21) */
void handle_conn_event (loop_t * loop, conn_t ** slot, uint32_t events) {
    if ((events & EPOLLERR) != 0) {
        destroy_conn(loop, slot, "reset");
        return;
    }
    switch ((*slot)->state) {
    case CONN_READ_HEAD:
    case CONN_IDLE:
        read_request_head(loop, slot);
        return;
    case CONN_SEND:
        send_response(loop, slot);
        return;
    case CONN_DISCARD:
        discard_request_body(loop, slot);
        return;
    case CONN_LINGER:
        read_lingering(loop, slot);
        return;
    case CONN_CONTINUE:
        send_continue(loop, slot);
        return;
    case CONN_READ_BODY:
        read_request_body(loop, slot);
        return;
    case CONN_CONNECT:
    case CONN_BACKEND_SEND:
    case CONN_BACKEND_READ:
        /* (G20, G21) 기다리는 중 손님이 떠남 → 백엔드도 같이 정리 */
        destroy_conn(loop, slot, "close");
        return;
    }
}

/* (L5, L6, L11, L12, L26, G11, G30) */
void expire_conn (loop_t * loop, conn_t ** slot) {
    switch ((*slot)->state) {
    case CONN_READ_HEAD:
        expire_header(loop, slot);
        return;
    case CONN_SEND:
    case CONN_CONTINUE:
        /* (L6) 쓰기 막힘은 말없이 닫는다 */
        destroy_conn(loop, slot, "timeout");
        return;
    case CONN_READ_BODY:
        expire_body(loop, slot);
        return;
    case CONN_IDLE:
    case CONN_DISCARD:
    case CONN_LINGER:
        destroy_conn(loop, slot, NULL);
        return;
    case CONN_CONNECT:
    case CONN_BACKEND_SEND:
    case CONN_BACKEND_READ:
        expire_backend(loop, slot);
        return;
    }
}

/* (L28, R6a, G20, G25, G26) close가 epoll에서도 뺀다 (fd를 dup하지 않으므로).
 * 같은 묶음의 다른 이벤트가 카드를 볼 수 있어 free는 미룬다 */
void destroy_conn (loop_t * loop, conn_t ** slot, const char * tag) {
    conn_t * conn = *slot;

    if (conn == NULL || conn->closed) {
        return;
    }
    log_request(loop, conn, tag);
    stop_timer(&conn->timer);
    release_backend(loop, conn);
    destroy_proxy(&conn->proxy);
    release_response(conn);
    destroy_request(&conn->request);
    close(conn->fd);
    conn->closed = true;
    conn->next_closed = loop->closed_conns;
    loop->closed_conns = conn;
    *slot = NULL;
    loop->destroyed_count++;
    report_card_counts(loop);
    release_connection_slot(loop);
}

/* (G25) */
void free_closed_conns (loop_t * loop) {
    while (loop->closed_conns != NULL) {
        conn_t * conn = loop->closed_conns;
        loop->closed_conns = conn->next_closed;
        free(conn);
    }
}

/* (E4) IDLE과 1바이트도 안 온 READ_HEAD는 바로 닫는다. 나머지는 요청의
 * keep-alive를 끄고(아직 안 만든 응답에 Connection: close), 이미 보내는 중인
 * 응답은 헤더를 바꿀 수 없어 보낸 뒤 닫는다. LINGER·DISCARD는 그대로 둔다 */
void shut_down_conn (loop_t * loop, conn_t ** slot) {
    conn_t * conn = *slot;

    if (conn->state == CONN_IDLE
        || (conn->state == CONN_READ_HEAD && !conn->has_received)) {
        destroy_conn(loop, slot, NULL);
        return;
    }
    conn->request.keep_alive = false;
    if (conn->state == CONN_SEND) {
        conn->close_after = true;
    }
}

/* (G12, G34, G36, L10) 본문을 다 받은 뒤라 연결 상태가 깨끗하다 */
void respond_with_proxy_status (loop_t * loop, conn_t ** slot, int status) {
    conn_t * conn = *slot;
    response_head_t head = {
        .status = status,
        .keep_alive = conn->request.keep_alive && status != 500,
    };

    conn->close_after = !head.keep_alive;
    start_response(loop, slot, &head);
}

/* (G10, G34, G40, G47) */
void start_proxy_response (loop_t * loop, conn_t ** slot, size_t head_length,
                           char * body, size_t body_start,
                           size_t body_length, int status) {
    conn_t * conn = *slot;

    conn->close_after = !conn->request.keep_alive;
    conn->response_body = body;
    conn->response_body_start = body_start;
    begin_response(loop, slot, head_length, status, body_length);
}

/* (G21) */
bool watch_client (loop_t * loop, conn_t * conn, uint32_t events) {
    struct epoll_event event = { .events = events, .data.ptr = conn };

    if (epoll_ctl(loop->epoll_fd, EPOLL_CTL_MOD, conn->fd, &event) != 0) {
        report_error("epoll_ctl");
        return false;
    }
    return true;
}

/* ── 도우미 함수 ── */

/* (R3c, L14, L30) 한 번 깨어날 때 읽기 1번. 연결 버퍼 남은 자리만큼만 */
static void read_request_head (loop_t * loop, conn_t ** slot) {
    conn_t * conn = *slot;
    ssize_t count = read(conn->fd, conn->buffer + conn->buffer_length,
                         MAX_HEADER_SIZE - conn->buffer_length);

    if (count < 0) {
        if (is_retry_error(errno)) {
            return;
        }
        destroy_conn(loop, slot, "reset");
        return;
    }
    if (count == 0) {
        destroy_conn(loop, slot, "close");
        return;
    }
    note_received(loop, conn);
    conn->buffer_length += (size_t) count;
    process_buffered_request(loop, slot);
}

/* (R3c) */
static bool is_retry_error (int error) {
    return error == EAGAIN || error == EWOULDBLOCK || error == EINTR;
}

/* (L5, L24, L37) keep-alive 대기 중 첫 바이트면 헤더 타이머를 이때 건다 */
static void note_received (loop_t * loop, conn_t * conn) {
    long long now = read_clock_ms();

    if (conn->has_received) {
        return;
    }
    conn->has_received = true;
    conn->request_started_ms = now;
    if (conn->state == CONN_IDLE) {
        conn->state = CONN_READ_HEAD;
        start_timer(&loop->header_timers, &conn->timer, now);
    }
}

/* (R1c, P3, P17, B29, L13) 헤더 영역이 완성됐을 때만 다음으로 간다 */
static void process_buffered_request (loop_t * loop, conn_t ** slot) {
    conn_t * conn = *slot;
    size_t header_length = 0;
    int status = 0;

    if (!skip_leading_empty_lines(conn->buffer, &conn->buffer_length)) {
        respond_with_error(loop, slot, 400);
        return;
    }
    find_result found = find_header_end(conn->buffer, conn->buffer_length,
                                        &header_length, &status);
    if (found == FIND_NEED_MORE) {
        return;
    }
    if (found == FIND_BAD) {
        respond_with_error(loop, slot, status);
        return;
    }
    start_request(loop, slot, header_length);
}

/* (B31, L6, L12, G45) 오류 응답 뒤에는 항상 lingering으로 닫는다 */
static void respond_with_error (loop_t * loop, conn_t ** slot, int status) {
    response_head_t head = { .status = status, .keep_alive = false };

    (*slot)->close_after = true;
    start_response(loop, slot, &head);
}

/* (R1f, L32) 헤더는 루프 버퍼에 쓰고 실제 길이만 카드에 복사한다 */
static void start_response (loop_t * loop, conn_t ** slot,
                            const response_head_t * head) {
    conn_t * conn = *slot;
    size_t length = write_response_head(head, time(NULL), loop->head);
    size_t body_length = 0;

    if (conn->result.fd >= 0) {
        body_length = conn->result.head.content_length;
    }
    begin_response(loop, slot, length, head->status, body_length);
}

/* (R1f, L32, G40) loop->head의 length바이트를 카드로 복사하고 SEND로 간다.
 * 할당에 실패하면 응답할 길이 없어 말없이 닫는다 */
static void begin_response (loop_t * loop, conn_t ** slot, size_t length,
                            int status, size_t body_length) {
    conn_t * conn = *slot;
    char * copy = copy_bytes(loop->head, length);

    if (copy == NULL) {
        report_error("response head");
        destroy_conn(loop, slot, "close");
        return;
    }
    conn->response_head = copy;
    conn->response_head_length = length;
    conn->response_head_sent = 0;
    conn->response_status = status;
    conn->body_length = body_length;
    conn->body_sent = 0;
    enter_state(loop, slot, CONN_SEND, EPOLLOUT, &loop->send_timers);
}

/* (L32) 반환: NULL = 할당 실패. 돌려받은 쪽이 free */
static char * copy_bytes (const char * source, size_t length) {
    char * copy = malloc(length > 0 ? length : 1);
    if (copy == NULL) {
        return NULL;
    }
    memcpy(copy, source, length);
    return copy;
}

/* (L15) 내부 오류는 stderr */
static void report_error (const char * what) {
    fprintf(stderr, "%s: %s\n", what, strerror(errno));
}

/* (L3, L13, L24) 상태마다 관심 이벤트와 타이머가 하나씩 정해진다.
 * epoll에 못 바꾸면 정리한다 (*slot이 NULL) */
static void enter_state (loop_t * loop, conn_t ** slot, conn_state_t state,
                         uint32_t events, timer_list_t * timers) {
    conn_t * conn = *slot;
    struct epoll_event event = { .events = events, .data.ptr = conn };

    if (epoll_ctl(loop->epoll_fd, EPOLL_CTL_MOD, conn->fd, &event) != 0) {
        report_error("epoll_ctl");
        destroy_conn(loop, slot, "close");
        return;
    }
    conn->state = state;
    start_timer(timers, &conn->timer, read_clock_ms());
}

/* (R1b, R2b, P13, P14, P25, B31, G3, G23) 파싱 → 라우팅.
 * 정리 오류(400/403/404)는 정적 쪽이 같은 정리로 같은 코드를 낸다 (G3, G37) */
static void start_request (loop_t * loop, conn_t ** slot,
                           size_t header_length) {
    conn_t * conn = *slot;
    const route_t * route = NULL;

    if (!copy_request_head(&conn->request, conn->buffer, header_length)) {
        respond_with_error(loop, slot, 500);
        return;
    }
    remove_buffer_prefix(conn->buffer, &conn->buffer_length, header_length);
    if (parse_request_head_limited(conn->request.head, header_length,
                                   loop->config->max_body,
                                   &conn->request) == PARSE_BAD) {
        respond_with_error(loop, slot, conn->request.status);
        return;
    }
    conn->has_parsed_request = true;
    if (loop->is_shutting_down) {
        /* (E4) 종료 중에 헤더가 완성된 요청도 응답 뒤 닫는다 */
        conn->request.keep_alive = false;
    }
    int status = find_route(loop->config, conn->request.path,
                            conn->request.path_length, &route);
    if (status == 200 && route != NULL) {
        start_proxy_request(loop, slot, route);
        return;
    }
    start_static_request(loop, slot);
}

/* (R2a, G7, G28, G33, G45) 요청 헤더를 미리 고치고 → 본문을 모으기 시작 →
 * Expect면 100 Continue. 431·500은 닫는다 */
static void start_proxy_request (loop_t * loop, conn_t ** slot,
                                 const route_t * route) {
    conn_t * conn = *slot;

    int status = create_proxy(loop, conn, route);
    if (status != 200) {
        respond_with_error(loop, slot, status);
        return;
    }
    if (start_collect_body(&conn->request, conn->buffer,
                           &conn->buffer_length) == BODY_NO_MEMORY) {
        respond_with_error(loop, slot, 500);
        return;
    }
    if (has_continue_expect(&conn->request)) {
        conn->continue_sent = 0;
        enter_state(loop, slot, CONN_CONTINUE, EPOLLOUT, &loop->send_timers);
        return;
    }
    receive_body_or_connect(loop, slot);
}

/* (G7, G38) 값이 100-continue일 때만 (대소문자 무시) */
static bool has_continue_expect (const request_t * request) {
    size_t position = 0;
    header_view_t line;

    while (next_header_line(request->headers, request->headers_length,
                            &position, &line) == LINE_OK) {
        if (line.name_length == 6 && strncasecmp(line.name, "Expect", 6) == 0
            && line.value_length == 12
            && strncasecmp(line.value, "100-continue", 12) == 0) {
            return true;
        }
    }
    return false;
}

/* (P11, G11) 본문을 다 받았으면 백엔드로, 아니면 본문 받기 */
static void receive_body_or_connect (loop_t * loop, conn_t ** slot) {
    conn_t * conn = *slot;

    if (conn->request.body_received == conn->request.content_length) {
        start_backend(loop, slot);
        return;
    }
    enter_state(loop, slot, CONN_READ_BODY, EPOLLIN, &loop->body_timers);
}

/* (R1d, L20, L21) 3단계 정적 처리 그대로. 본문은 응답 뒤에 버린다.
 * 닫을 연결이면 lingering이 같이 버린다 */
static void start_static_request (loop_t * loop, conn_t ** slot) {
    conn_t * conn = *slot;

    open_static_file(&conn->request, loop->config->root, &conn->result);
    decide_connection_close(conn);
    if (!conn->close_after && conn->request.content_length > 0) {
        start_discard_body(&conn->request, conn->buffer,
                           &conn->buffer_length);
    }
    start_response(loop, slot, &conn->result.head);
}

/* (L10, L11, L27) 2단계 결과의 keep_alive를 연결 관리 쪽에서 덮어쓴다.
 * 손님 half-close로는 바꾸지 않는다 (L27) */
static void decide_connection_close (conn_t * conn) {
    response_head_t * head = &conn->result.head;

    if (head->status == 400 || head->status == 500) {
        head->keep_alive = false;
    }
    if (has_expect_header(&conn->request)) {
        head->keep_alive = false;
    }
    conn->close_after = !head->keep_alive;
}

/* (L11, L21, P13) */
static bool has_expect_header (const request_t * request) {
    size_t position = 0;
    header_view_t line;

    while (next_header_line(request->headers, request->headers_length,
                            &position, &line) == LINE_OK) {
        if (line.name_length == 6
            && strncasecmp(line.name, "Expect", 6) == 0) {
            return true;
        }
    }
    return false;
}

/* (R1f, R3c, L30, G40) 한 번 깨어날 때 쓰기 1번: 헤더 남은 것,
 * 아니면 본문 조각 하나 (메모리 또는 파일) */
static void send_response (loop_t * loop, conn_t ** slot) {
    conn_t * conn = *slot;

    if (conn->response_head_sent < conn->response_head_length) {
        send_head_part(loop, slot);
        return;
    }
    if (conn->response_body != NULL) {
        send_memory_chunk(loop, slot);
        return;
    }
    send_file_chunk(loop, slot);
}

/* (R3c) */
static void send_head_part (loop_t * loop, conn_t ** slot) {
    conn_t * conn = *slot;
    ssize_t written = write(conn->fd,
                            conn->response_head + conn->response_head_sent,
                            conn->response_head_length
                            - conn->response_head_sent);

    if (written < 0) {
        handle_write_error(loop, slot);
        return;
    }
    conn->response_head_sent += (size_t) written;
    note_progress(loop, slot, (size_t) written);
}

/* (R6a, L15, L17) EPIPE·ECONNRESET은 손님이 떠난 것 */
static void handle_write_error (loop_t * loop, conn_t ** slot) {
    if (is_retry_error(errno)) {
        return;
    }
    destroy_conn(loop, slot, "reset");
}

/* (L5) 쓰기 타임아웃은 "사이"라 쓸 때마다 다시 건다 */
static void note_progress (loop_t * loop, conn_t ** slot, size_t written) {
    conn_t * conn = *slot;

    conn->bytes_sent += written;
    if (is_response_sent(conn)) {
        finish_response(loop, slot);
        return;
    }
    start_timer(&loop->send_timers, &conn->timer, read_clock_ms());
}

/* (R1f, L16) */
static bool is_response_sent (const conn_t * conn) {
    return conn->response_head_sent == conn->response_head_length
           && conn->body_sent == conn->body_length;
}

/* (L12, L13, L15, L21) 응답 끝: 닫기 / 본문 버리기 / 다음 요청 */
static void finish_response (loop_t * loop, conn_t ** slot) {
    conn_t * conn = *slot;
    const char * tag = conn->close_after ? "close" : NULL;

    if (conn->finish_tag != NULL) {
        tag = conn->finish_tag;
    }
    log_request(loop, conn, tag);
    release_response(conn);
    if (conn->close_after) {
        start_lingering(loop, slot);
        return;
    }
    if (conn->request.discard_remaining > 0) {
        start_discarding(loop, slot);
        return;
    }
    finish_request(loop, slot);
}

/* (L15, L26, L37, G18) 요청 하나에 한 줄. 받은 바이트가 0이면 찍지 않는다 */
static void log_request (loop_t * loop, conn_t * conn, const char * tag) {
    if (conn->is_logged || !conn->has_received) {
        return;
    }
    conn->is_logged = true;
    if (!loop->config->access_log) {
        return;
    }
    access_entry_t entry = {
        .peer = &conn->peer,
        .request_line = NULL,
        .request_line_length = 0,
        .status = conn->response_status,
        .bytes_sent = conn->bytes_sent,
        .elapsed_ms = read_clock_ms() - conn->request_started_ms,
        .tag = tag,
    };
    if (conn->has_parsed_request) {
        entry.request_line = conn->request.head;
        entry.request_line_length = find_request_line_length(&conn->request);
    }
    add_backend_entry(loop, conn, &entry);
    write_access_log(&entry);
}

/* (L37) 파싱에 성공한 헤더 복사본의 첫 줄 길이 ('\r' 앞까지) */
static size_t find_request_line_length (const request_t * request) {
    const char * end = memchr(request->head, '\r', request->head_length);

    if (end == NULL) {
        return request->head_length;
    }
    return (size_t) (end - request->head);
}

/* (G18, G39) connect를 한 번이라도 한 요청만. 백엔드를 아직 쥐고 있으면
 * 지금까지 걸린 시간 */
static void add_backend_entry (const loop_t * loop, const conn_t * conn,
                               access_entry_t * entry) {
    const proxy_t * proxy = &conn->proxy;
    long long end_ms = proxy->fd >= 0 ? read_clock_ms() : proxy->finished_ms;

    if (proxy->route == NULL || proxy->attempts == 0) {
        return;
    }
    entry->backend = find_backend_address(loop->upstream, proxy->route,
                                          proxy->position);
    entry->retry_count = proxy->attempts - 1;
    entry->backend_ms = end_ms - proxy->started_ms;
    entry->backend_cause = proxy->cause;
}

/* (L32, S11, G47) 두 번 불러도 안전 */
static void release_response (conn_t * conn) {
    free(conn->response_head);
    conn->response_head = NULL;
    free(conn->response_body);
    conn->response_body = NULL;
    close_static_file(&conn->result);
}

/* (L12) 보낼 것은 다 보냈다고 알리고, 들어오는 것은 읽어서 버린다 */
static void start_lingering (loop_t * loop, conn_t ** slot) {
    conn_t * conn = *slot;

    shutdown(conn->fd, SHUT_WR);
    conn->total_deadline_ms = read_clock_ms() + loop->config->linger_total_ms;
    enter_state(loop, slot, CONN_LINGER, EPOLLIN, &loop->quiet_timers);
}

/* (L11, L21) */
static void start_discarding (loop_t * loop, conn_t ** slot) {
    conn_t * conn = *slot;

    conn->total_deadline_ms = read_clock_ms() + loop->config->linger_total_ms;
    enter_state(loop, slot, CONN_DISCARD, EPOLLIN, &loop->quiet_timers);
}

/* (R3d, L13, E4) epoll은 커널 버퍼만 보므로 우리 버퍼를 먼저 확인한다.
 * 종료 중에는 다음 요청을 받지 않고 닫는다 (본문 버리기가 끝난 경우) */
static void finish_request (loop_t * loop, conn_t ** slot) {
    conn_t * conn = *slot;

    if (loop->is_shutting_down) {
        destroy_conn(loop, slot, NULL);
        return;
    }
    reset_request(conn);
    if (conn->buffer_length == 0) {
        enter_state(loop, slot, CONN_IDLE, EPOLLIN, &loop->idle_timers);
        return;
    }
    conn->has_received = true;
    conn->request_started_ms = read_clock_ms();
    enter_state(loop, slot, CONN_READ_HEAD, EPOLLIN,
                &loop->header_timers);
    if (*slot == NULL) {
        return;
    }
    process_buffered_request(loop, slot);
}

/* (P24, L24, G26) 다음 요청을 위해 요청 하나 몫의 필드를 비운다.
 * 백엔드 연결은 응답을 다 받을 때 이미 놓았다 */
static void reset_request (conn_t * conn) {
    destroy_request(&conn->request);
    destroy_proxy(&conn->proxy);
    conn->has_parsed_request = false;
    conn->has_received = false;
    conn->is_logged = false;
    conn->finish_tag = NULL;
    conn->response_status = 0;
    conn->bytes_sent = 0;
    conn->body_length = 0;
    conn->body_sent = 0;
    conn->response_body_start = 0;
    conn->continue_sent = 0;
    conn->response_head_length = 0;
    conn->response_head_sent = 0;
}

/* (G40) 메모리 본문도 L30처럼 한 번에 8KB 조각 하나 */
static void send_memory_chunk (loop_t * loop, conn_t ** slot) {
    conn_t * conn = *slot;
    size_t wanted = pick_chunk_size(conn->body_length - conn->body_sent);
    ssize_t written = write(conn->fd,
                            conn->response_body + conn->response_body_start
                            + conn->body_sent, wanted);

    if (written < 0) {
        handle_write_error(loop, slot);
        return;
    }
    conn->body_sent += (size_t) written;
    note_progress(loop, slot, (size_t) written);
}

/* (L9, L16, G40) Content-Length보다 더 읽지 않는다 */
static size_t pick_chunk_size (size_t remaining) {
    return remaining < FILE_CHUNK_SIZE ? remaining : FILE_CHUNK_SIZE;
}

/* (L9, L16) 보낸 위치부터 pread. 덜 보낸 부분은 다음에 다시 읽는다.
 * 파일이 약속보다 먼저 끝나면(줄어듦) 닫는다 */
static void send_file_chunk (loop_t * loop, conn_t ** slot) {
    conn_t * conn = *slot;
    size_t wanted = pick_chunk_size(conn->body_length - conn->body_sent);
    ssize_t read_count = pread(conn->result.fd, loop->chunk, wanted,
                               (off_t) conn->body_sent);

    if (read_count < 0) {
        report_error("pread");
        destroy_conn(loop, slot, "close");
        return;
    }
    if (read_count == 0) {
        destroy_conn(loop, slot, "close");
        return;
    }
    ssize_t written = write(conn->fd, loop->chunk, (size_t) read_count);
    if (written < 0) {
        handle_write_error(loop, slot);
        return;
    }
    conn->body_sent += (size_t) written;
    note_progress(loop, slot, (size_t) written);
}

/* (L11, L14, L20, L33) 남은 본문만큼만 읽어서 다음 요청 경계를 지킨다 */
static void discard_request_body (loop_t * loop, conn_t ** slot) {
    conn_t * conn = *slot;
    size_t wanted = pick_chunk_size(conn->request.discard_remaining);
    ssize_t count = read(conn->fd, loop->chunk, wanted);

    if (count < 0) {
        if (is_retry_error(errno)) {
            return;
        }
        destroy_conn(loop, slot, "reset");
        return;
    }
    if (count == 0) {
        destroy_conn(loop, slot, "close");
        return;
    }
    if (add_body_bytes(&conn->request, (size_t) count) == BODY_DONE) {
        finish_request(loop, slot);
        return;
    }
    keep_reading_quietly(loop, slot);
}

/* (L11, L12, L29) 조용 타이머는 읽을 때마다 다시 걸고,
 * 전체 상한은 손님이 계속 보내서 읽기가 올 때 확인한다 */
static void keep_reading_quietly (loop_t * loop, conn_t ** slot) {
    conn_t * conn = *slot;
    long long now = read_clock_ms();

    if (now >= conn->total_deadline_ms) {
        destroy_conn(loop, slot, NULL);
        return;
    }
    start_timer(&loop->quiet_timers, &conn->timer, now);
}

/* (L12, L14, L33) */
static void read_lingering (loop_t * loop, conn_t ** slot) {
    conn_t * conn = *slot;
    ssize_t count = read(conn->fd, loop->chunk, FILE_CHUNK_SIZE);

    if (count < 0) {
        if (is_retry_error(errno)) {
            return;
        }
        destroy_conn(loop, slot, "reset");
        return;
    }
    if (count == 0) {
        destroy_conn(loop, slot, NULL);
        return;
    }
    keep_reading_quietly(loop, slot);
}

/* (G7, G38, L5) 부분 쓰기를 이어 간다. 다 보내면 본문 받기 또는 백엔드로 */
static void send_continue (loop_t * loop, conn_t ** slot) {
    conn_t * conn = *slot;
    ssize_t written = write(conn->fd, CONTINUE_LINE + conn->continue_sent,
                            CONTINUE_LINE_LENGTH - conn->continue_sent);

    if (written < 0) {
        handle_write_error(loop, slot);
        return;
    }
    conn->continue_sent += (size_t) written;
    if (conn->continue_sent < CONTINUE_LINE_LENGTH) {
        start_timer(&loop->send_timers, &conn->timer, read_clock_ms());
        return;
    }
    receive_body_or_connect(loop, slot);
}

/* (R2a, R3c, P11, P26, L5, L14, L30) 한 번 깨어날 때 읽기 1번,
 * 남은 본문만큼만. 본문 타임아웃은 "사이"라 읽을 때마다 다시 건다 */
static void read_request_body (loop_t * loop, conn_t ** slot) {
    conn_t * conn = *slot;
    request_t * request = &conn->request;
    ssize_t count = read(conn->fd, request->body + request->body_received,
                         request->content_length - request->body_received);

    if (count < 0) {
        if (is_retry_error(errno)) {
            return;
        }
        destroy_conn(loop, slot, "reset");
        return;
    }
    if (count == 0) {
        destroy_conn(loop, slot, "close");
        return;
    }
    if (add_body_bytes(request, (size_t) count) == BODY_DONE) {
        start_backend(loop, slot);
        return;
    }
    start_timer(&loop->body_timers, &conn->timer, read_clock_ms());
}

/* (L6, L26) 1바이트라도 받았으면 408, 아니면 말없이 닫는다 */
static void expire_header (loop_t * loop, conn_t ** slot) {
    if (!(*slot)->has_received) {
        destroy_conn(loop, slot, NULL);
        return;
    }
    (*slot)->finish_tag = "timeout";
    respond_with_error(loop, slot, 408);
}

/* (L5, L6) 본문 수신 중 타임아웃은 408 + close */
static void expire_body (loop_t * loop, conn_t ** slot) {
    (*slot)->finish_tag = "timeout";
    respond_with_error(loop, slot, 408);
}

/* (L19, L36) */
static void report_card_counts (const loop_t * loop) {
    if (!loop->config->count_log) {
        return;
    }
    fprintf(stderr, "cards created=%llu destroyed=%llu\n",
            loop->created_count, loop->destroyed_count);
}
