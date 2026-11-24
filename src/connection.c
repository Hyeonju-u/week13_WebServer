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
#include "http_response.h"

static void read_request_head (loop_t * loop, conn_t ** slot);
static bool is_retry_error (int error);
static void note_received (loop_t * loop, conn_t * conn);
static void process_buffered_request (loop_t * loop, conn_t ** slot);
static void respond_with_error (loop_t * loop, conn_t ** slot, int status);
static void start_response (loop_t * loop, conn_t ** slot,
                            const response_head_t * head);
static char * copy_bytes (const char * source, size_t length);
static void report_error (const char * what);
static void enter_state (loop_t * loop, conn_t ** slot, conn_state_t state,
                         uint32_t events, timer_list_t * timers);
static void start_request (loop_t * loop, conn_t ** slot,
                           size_t header_length);
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
static void release_response (conn_t * conn);
static void start_lingering (loop_t * loop, conn_t ** slot);
static void start_discarding (loop_t * loop, conn_t ** slot);
static void finish_request (loop_t * loop, conn_t ** slot);
static void reset_request (conn_t * conn);
static void send_file_chunk (loop_t * loop, conn_t ** slot);
static size_t pick_chunk_size (size_t remaining);
static void discard_request_body (loop_t * loop, conn_t ** slot);
static void keep_reading_quietly (loop_t * loop, conn_t ** slot);
static void read_lingering (loop_t * loop, conn_t ** slot);
static void expire_header (loop_t * loop, conn_t ** slot);
static void report_card_counts (const loop_t * loop);

/* ── 공개 함수 ── */

/* (L28) 카드가 커서(연결 버퍼 8KB) 복합 리터럴 대신 memset으로 비운다 */
conn_t * create_conn (int fd, const struct sockaddr_in * peer) {
    conn_t * conn = malloc(sizeof(*conn));
    if (conn == NULL) {
        return NULL;
    }
    memset(conn, 0, sizeof(*conn));
    conn->fd = fd;
    conn->peer = *peer;
    conn->state = CONN_READ_HEAD;
    conn->result.fd = -1;
    conn->timer.owner = conn;
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

/* (L3, L24) 손님 half-close는 따로 보지 않고 read 0으로 안다 (L14, L27) */
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
    }
}

/* (L5, L6, L11, L12, L26) */
void expire_conn (loop_t * loop, conn_t ** slot) {
    switch ((*slot)->state) {
    case CONN_READ_HEAD:
        expire_header(loop, slot);
        return;
    case CONN_SEND:
        /* (L6) 쓰기 막힘은 말없이 닫는다 */
        destroy_conn(loop, slot, "timeout");
        return;
    case CONN_IDLE:
    case CONN_DISCARD:
    case CONN_LINGER:
        destroy_conn(loop, slot, NULL);
        return;
    }
}

/* (L28, R6a) close가 epoll에서도 뺀다 (fd를 dup하지 않으므로) */
void destroy_conn (loop_t * loop, conn_t ** slot, const char * tag) {
    conn_t * conn = *slot;

    if (conn == NULL) {
        return;
    }
    log_request(loop, conn, tag);
    stop_timer(&conn->timer);
    release_response(conn);
    destroy_request(&conn->request);
    close(conn->fd);
    free(conn);
    *slot = NULL;
    loop->destroyed_count++;
    report_card_counts(loop);
    release_connection_slot(loop);
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

/* (B31, L6, L12) 오류 응답 뒤에는 항상 lingering으로 닫는다 */
static void respond_with_error (loop_t * loop, conn_t ** slot, int status) {
    response_head_t head = { .status = status, .keep_alive = false };

    (*slot)->close_after = true;
    start_response(loop, slot, &head);
}

/* (R1f, L32) 헤더는 루프 버퍼에 쓰고 실제 길이만 카드에 복사한다.
 * 할당에 실패하면 응답할 길이 없어 말없이 닫는다 */
static void start_response (loop_t * loop, conn_t ** slot,
                            const response_head_t * head) {
    conn_t * conn = *slot;
    size_t length = write_response_head(head, time(NULL), loop->head);
    char * copy = copy_bytes(loop->head, length);

    if (copy == NULL) {
        report_error("response head");
        destroy_conn(loop, slot, "close");
        return;
    }
    conn->response_head = copy;
    conn->response_head_length = length;
    conn->response_head_sent = 0;
    conn->response_status = head->status;
    conn->file_length = 0;
    if (conn->result.fd >= 0) {
        conn->file_length = conn->result.head.content_length;
    }
    conn->file_sent = 0;
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

/* (R1b, R1d, P13, P14, P25, B31, L20, L21) 3단계는 모든 요청이 정적이다.
 * 본문은 응답 뒤에 버린다. 닫을 연결이면 lingering이 같이 버린다 */
static void start_request (loop_t * loop, conn_t ** slot,
                           size_t header_length) {
    conn_t * conn = *slot;

    if (!copy_request_head(&conn->request, conn->buffer, header_length)) {
        respond_with_error(loop, slot, 500);
        return;
    }
    remove_buffer_prefix(conn->buffer, &conn->buffer_length, header_length);
    if (parse_request_head(conn->request.head, header_length,
                           &conn->request) == PARSE_BAD) {
        respond_with_error(loop, slot, conn->request.status);
        return;
    }
    conn->has_parsed_request = true;
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

/* (R1f, R3c, L30) 한 번 깨어날 때 쓰기 1번: 헤더 남은 것, 아니면 조각 하나 */
static void send_response (loop_t * loop, conn_t ** slot) {
    conn_t * conn = *slot;

    if (conn->response_head_sent < conn->response_head_length) {
        send_head_part(loop, slot);
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
           && conn->file_sent == conn->file_length;
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

/* (L15, L26, L37) 요청 하나에 한 줄. 받은 바이트가 0이면 찍지 않는다 */
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

/* (L32, S11) 두 번 불러도 안전 */
static void release_response (conn_t * conn) {
    free(conn->response_head);
    conn->response_head = NULL;
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

/* (R3d, L13) epoll은 커널 버퍼만 보므로 우리 버퍼를 먼저 확인한다 */
static void finish_request (loop_t * loop, conn_t ** slot) {
    conn_t * conn = *slot;

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

/* (P24, L24) 다음 요청을 위해 요청 하나 몫의 필드를 비운다 */
static void reset_request (conn_t * conn) {
    destroy_request(&conn->request);
    conn->has_parsed_request = false;
    conn->has_received = false;
    conn->is_logged = false;
    conn->finish_tag = NULL;
    conn->response_status = 0;
    conn->bytes_sent = 0;
    conn->file_length = 0;
    conn->file_sent = 0;
    conn->response_head_length = 0;
    conn->response_head_sent = 0;
}

/* (L9, L16) 보낸 위치부터 pread. 덜 보낸 부분은 다음에 다시 읽는다.
 * 파일이 약속보다 먼저 끝나면(줄어듦) 닫는다 */
static void send_file_chunk (loop_t * loop, conn_t ** slot) {
    conn_t * conn = *slot;
    size_t wanted = pick_chunk_size(conn->file_length - conn->file_sent);
    ssize_t read_count = pread(conn->result.fd, loop->chunk, wanted,
                               (off_t) conn->file_sent);

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
    conn->file_sent += (size_t) written;
    note_progress(loop, slot, (size_t) written);
}

/* (L9, L16) Content-Length보다 더 읽지 않는다 */
static size_t pick_chunk_size (size_t remaining) {
    return remaining < FILE_CHUNK_SIZE ? remaining : FILE_CHUNK_SIZE;
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

/* (L6, L26) 1바이트라도 받았으면 408, 아니면 말없이 닫는다 */
static void expire_header (loop_t * loop, conn_t ** slot) {
    if (!(*slot)->has_received) {
        destroy_conn(loop, slot, NULL);
        return;
    }
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
