#include "server.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include "http_parser.h"
#include "http_response.h"
#include "log.h"
#include "proxy.h"
#include "settings.h"
#include "signals.h"
#include "static_file.h"

#define EVENT_BATCH 1024
#define WAIT_MAX_MS 1000
#define RESERVE_PATH "/dev/null"
#define DISCARD_CHUNK_BYTES 4096
#define DISCARD_ROUNDS_MAX 256

static int prepare_server (server_t * server,
    const server_options_t * options);
static int open_root_directory (server_t * server, const char * root_path);
static void raise_fd_limit (size_t max_connections);
static void apply_fd_limit (struct rlimit * limit, rlim_t wanted);
static int create_listen_socket (int port);
static void begin_shutdown (server_t * server);
static void prepare_for_shutdown (server_t * server, conn_t * conn);
static int is_finished (const server_t * server);
static int compute_wait_timeout (const server_t * server);
static void process_pending_requests (server_t * server);
static void unlink_pending (server_t * server, conn_t * conn);
static void process_buffered_request (server_t * server, conn_t * conn);
static void reject_request (server_t * server, conn_t * conn,
    int status_code);
static void wait_for_more_input (server_t * server, conn_t * conn,
    const http_request_t * request);
static void handle_request (server_t * server, conn_t * conn,
    const http_request_t * request);
static void start_static_response (server_t * server, conn_t * conn,
    const http_request_t * request);
static void queue_file_response (server_t * server, conn_t * conn,
    const static_file_t * file);
static void start_sending (server_t * server, conn_t * conn);
static int fill_output_from_file (conn_t * conn);
static void discard_file_response (server_t * server, conn_t * conn);
static void queue_redirect_response (server_t * server, conn_t * conn,
    const http_request_t * request);
static char * create_location_header (const char * uri, size_t path_length,
    size_t uri_length);
static void dispatch_event (server_t * server, const event_t * event);
static void accept_connections (server_t * server);
static void pause_accepting (server_t * server);
static int accept_one_connection (server_t * server);
static int handle_accept_failure (server_t * server);
static void discard_waiting_connection (server_t * server);
static void register_client (server_t * server, int fd,
    const struct sockaddr_storage * address);
static void remember_client_address (conn_t * conn,
    const struct sockaddr_storage * address);
static void handle_client_event (server_t * server, conn_t * conn,
    const event_t * event);
static void read_from_client (server_t * server, conn_t * conn);
static int ensure_input_space (conn_t * conn);
static void handle_input_shortage (server_t * server, conn_t * conn,
    int status_code);
static void handle_client_data (server_t * server, conn_t * conn);
static void handle_client_eof (server_t * server, conn_t * conn);
static void handle_read_error (server_t * server, conn_t * conn);
static void write_to_client (server_t * server, conn_t * conn);
static void update_after_write (server_t * server, conn_t * conn);
static void handle_write_error (server_t * server, conn_t * conn);
static void expire_timers (server_t * server);
static void handle_timeout (server_t * server, conn_t * conn);
static void resume_accepting (server_t * server);
static void free_closed_connections (server_t * server);
static void link_connection (server_t * server, conn_t * conn);
static void close_peer (server_t * server, conn_t * conn);
static void discard_unread_bytes (int fd);
static void unlink_connection (server_t * server, conn_t * conn);
static void compute_interest (const conn_t * conn, int * wants_read,
    int * wants_write);
static void reset_request_state (conn_t * conn);
static void queue_pending (server_t * server, conn_t * conn);

/* ── 공개 함수 ── */

server_t * create_server (const server_options_t * options) {
    server_t * server = calloc(1, sizeof(*server));
    if (server == NULL) {
        return NULL;
    }
    server->listen_fd = -1;
    server->reserve_fd = -1;
    server->root_fd = -1;
    server->proxy_config = options->proxy_config;
    server->max_connections = options->max_connections;
    server->now_ms = get_now_ms();
    server->upstream = create_upstream(options->proxy_config);
    if (server->upstream == NULL || prepare_server(server, options) != 0) {
        destroy_server(server);
        return NULL;
    }
    return server;
}

/*
 * 이벤트 루프 한 바퀴: 대기 → 처리 대기 요청 → 이벤트 → 타임아웃 → 정리.
 * 루프는 wait_events에서만 기다린다 (CON-06).
 */
int run_server (server_t * server) {
    event_t events[EVENT_BATCH];
    for (;;) {
        if (is_shutdown_requested() && !server->is_shutting_down) {
            begin_shutdown(server);
        }
        if (is_finished(server)) {
            break;
        }
        int count = wait_events(server->loop, events, EVENT_BATCH,
            compute_wait_timeout(server));
        if (count < 0 && errno != EINTR) {
            write_log(LOG_ERROR, "이벤트 대기 실패: %s", strerror(errno));
            return -1;
        }
        server->now_ms = get_now_ms();
        process_pending_requests(server);
        for (int i = 0; i < count; i++) {
            dispatch_event(server, &events[i]);
        }
        expire_timers(server);
        resume_accepting(server);
        free_closed_connections(server);
    }
    return 0;
}

void destroy_server (server_t * server) {
    while (server->all_head != NULL) {
        close_connection(server, server->all_head);
    }
    free_closed_connections(server);
    if (server->listen_fd >= 0) {
        close(server->listen_fd);
    }
    if (server->reserve_fd >= 0) {
        close(server->reserve_fd);
    }
    if (server->root_fd >= 0) {
        close(server->root_fd);
    }
    if (server->loop != NULL) {
        destroy_event_loop(server->loop);
    }
    if (server->heap != NULL) {
        destroy_timer_heap(server->heap);
    }
    destroy_upstream(server->upstream);
    free(server);
}

/* 실패하면 conn은 등록되지 않은 채 남으므로 호출자가 destroy한다 */
int register_connection (server_t * server, conn_t * conn) {
    if (add_event(server->loop, conn->fd, conn, conn->wants_read,
            conn->wants_write) != 0) {
        return -1;
    }
    link_connection(server, conn);
    return 0;
}

/*
 * 모든 정리 경로가 여기로 모인다: close(fd)가 정확히 한 번, 감시 목록에서
 * 먼저 제거, 구조체 free는 이번 바퀴가 끝난 뒤 (NET-15, NET-16, NET-17).
 */
void close_connection (server_t * server, conn_t * conn) {
    if (conn->is_closed) {
        return;
    }
    conn->is_closed = 1;
    conn->state = STATE_CLOSED;
    close_peer(server, conn);
    delete_event(server->loop, conn->fd);
    discard_unread_bytes(conn->fd);
    close(conn->fd);
    conn->fd = -1;
    if (conn->file_fd >= 0) {
        close(conn->file_fd);
        conn->file_fd = -1;
    }
    remove_from_heap(server->heap, conn);
    unlink_connection(server, conn);
    unlink_pending(server, conn);
    if (conn->kind == CONN_CLIENT) {
        server->connection_count--;
    }
    release_buffer(&conn->input);
    release_buffer(&conn->output);
    release_buffer(&conn->forward);
    conn->closed_next = server->closed_head;
    server->closed_head = conn;
}

/* 종료 중에는 어떤 마감도 종료 마감보다 늦을 수 없다 (CON-15) */
void set_connection_deadline (server_t * server, conn_t * conn,
    deadline_kind_t kind, long long timeout_ms) {
    long long deadline_ms = server->now_ms + timeout_ms;
    if (server->is_shutting_down
        && deadline_ms > server->shutdown_deadline_ms) {
        kind = DEADLINE_SHUTDOWN;
        deadline_ms = server->shutdown_deadline_ms;
    }
    conn->deadline_kind = kind;
    if (set_heap_deadline(server->heap, conn, deadline_ms) != 0) {
        write_log(LOG_ERROR, "타임아웃 등록 실패: 메모리 부족");
    }
}

void clear_connection_deadline (server_t * server, conn_t * conn) {
    if (server->is_shutting_down) {
        set_connection_deadline(server, conn, DEADLINE_SHUTDOWN,
            SHUTDOWN_WAIT_MS);
        return;
    }
    remove_from_heap(server->heap, conn);
    conn->deadline_kind = DEADLINE_NONE;
}

/* 보낼 것이 있을 때만 쓰기 대기를 켠다. 아니면 루프가 CPU 100%로 돈다 */
void update_interest (server_t * server, conn_t * conn) {
    if (conn->is_closed) {
        return;
    }
    int wants_read = 0;
    int wants_write = 0;
    compute_interest(conn, &wants_read, &wants_write);
    if (wants_read == conn->wants_read && wants_write == conn->wants_write) {
        return;
    }
    if (modify_event(server->loop, conn->fd, conn, wants_read,
            wants_write) != 0) {
        write_log(LOG_ERROR, "이벤트 변경 실패: %s", strerror(errno));
        close_connection(server, conn);
        return;
    }
    conn->wants_read = wants_read;
    conn->wants_write = wants_write;
}

/* 연결 유지 여부와 HEAD 여부는 conn에 미리 정해 둔 값을 쓴다 */
void queue_error_response (server_t * server, conn_t * client, int status_code,
    const char * extra_headers) {
    if (append_error_response(&client->output, status_code,
            client->keep_alive, client->is_head, extra_headers) != 0) {
        write_log(LOG_ERROR, "오류 응답 생성 실패: 메모리 부족");
        close_connection(server, client);
        return;
    }
    start_sending(server, client);
}

/* 다음 요청을 받을 준비를 하거나 연결을 닫는다 (RES-12) */
void finish_response (server_t * server, conn_t * client) {
    reset_request_state(client);
    if (!client->keep_alive || server->is_shutting_down) {
        close_connection(server, client);
        return;
    }
    client->state = STATE_READING;
    if (client->input.length > 0) {
        set_connection_deadline(server, client, DEADLINE_REQUEST,
            REQUEST_TIMEOUT_MS);
        queue_pending(server, client);
    } else if (client->is_half_closed) {
        close_connection(server, client);
        return;
    } else {
        set_connection_deadline(server, client, DEADLINE_IDLE,
            KEEP_ALIVE_TIMEOUT_MS);
    }
    update_interest(server, client);
}

int set_nonblocking (int fd) {
    int flags = fcntl(fd, F_GETFL);
    if (flags < 0) {
        return -1;
    }
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/* ── 도우미 함수 ── */

static int prepare_server (server_t * server,
    const server_options_t * options) {
    if (open_root_directory(server, options->root_path) != 0) {
        return -1;
    }
    raise_fd_limit(options->max_connections);
    server->loop = create_event_loop();
    server->heap = create_timer_heap();
    if (server->loop == NULL || server->heap == NULL) {
        return -1;
    }
    server->listen_fd = create_listen_socket(options->port);
    if (server->listen_fd < 0) {
        return -1;
    }
    server->reserve_fd = open(RESERVE_PATH, O_RDONLY);
    if (server->reserve_fd < 0) {
        write_log(LOG_ERROR, "비상용 fd를 열 수 없음: %s", strerror(errno));
        return -1;
    }
    if (add_event(server->loop, server->listen_fd, server, 1, 0) != 0) {
        write_log(LOG_ERROR, "listen 소켓 등록 실패: %s", strerror(errno));
        return -1;
    }
    server->is_accepting = 1;
    return 0;
}

/* 루트는 시작할 때 한 번 열어 fd를 보관한다. 이후 모든 경로 검사의 기준이다 */
static int open_root_directory (server_t * server, const char * root_path) {
    int fd = open(root_path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        write_log(LOG_ERROR, "루트를 열 수 없음 %s: %s", root_path,
            strerror(errno));
        return -1;
    }
    struct stat info;
    if (fstat(fd, &info) != 0 || !S_ISDIR(info.st_mode)) {
        write_log(LOG_ERROR, "루트가 폴더가 아님: %s", root_path);
        close(fd);
        return -1;
    }
    server->root_fd = fd;
    server->root_path_length = strlen(root_path);
    return 0;
}

/*
 * 손님 소켓 말고도 파일 fd, 백엔드 소켓, 비상용 fd가 필요해서 최대 연결 수의
 * 두 배를 목표로 삼는다. macOS 기본 한도(256)로는 1만 연결을 받을 수 없다.
 */
static void raise_fd_limit (size_t max_connections) {
    struct rlimit limit;
    //getrlimit:열수있는 파일(fd)개수를 limit 그릇에 담아줘 반환값 0 성공 -1 실패  
    if (getrlimit(RLIMIT_NOFILE, &limit) != 0) {
        return;
    }
    /*연결 1개당 fd2개,(손님소켓+백엔드 소켓 또는 보내는 중인파일)
    +64 = 리슨소켓,kqueue/epoll,루트 fd,비상용 fd,같은 고정 fd 여유분 */
    rlim_t wanted = (rlim_t) max_connections * 2 + 64;
    //지금한도 (soft)가 목표보다 낮을때만 올림(이미 충분하면 건드리지않음)
    if (limit.rlim_cur < wanted) {
        apply_fd_limit(&limit, wanted);//안에서 Hard 한도를 넘지 않게 조정후 setrlimit 할 것
    }
    getrlimit(RLIMIT_NOFILE, &limit);
    write_log(LOG_INFO, "fd 한도 %llu, 최대 연결 수 %zu",
        (unsigned long long) limit.rlim_cur, max_connections);
    if (limit.rlim_cur < wanted) {
        write_log(LOG_INFO, "fd 한도가 목표 %llu보다 낮아 연결이 몰리면 "
            "파일을 열지 못해 500이 나갈 수 있음", (unsigned long long) wanted);
    }
}

/* 목표가 거절되면 macOS의 OPEN_MAX(10240)까지만이라도 올려 본다 */
static void apply_fd_limit (struct rlimit * limit, rlim_t wanted) {
    rlim_t fallback = 10240;
    rlim_t hard = limit->rlim_max;
    limit->rlim_cur = wanted > hard ? hard : wanted;
    if (setrlimit(RLIMIT_NOFILE, limit) == 0) {
        return;
    }
    limit->rlim_cur = fallback > hard ? hard : fallback;
    setrlimit(RLIMIT_NOFILE, limit);
}

static int create_listen_socket (int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        write_log(LOG_ERROR, "socket 실패: %s", strerror(errno));
        return -1;
    }
    int enabled = 1;
    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons((unsigned short) port);
    /* 재시작할 때 이전 연결의 TIME_WAIT에 막히지 않도록 bind 전에 설정한다 */
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enabled,
            sizeof(enabled)) != 0
        || bind(fd, (struct sockaddr *) &address, sizeof(address)) != 0
        || listen(fd, SOMAXCONN) != 0 || set_nonblocking(fd) != 0) {
        write_log(LOG_ERROR, "포트 %d 준비 실패: %s", port, strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

/* 종료 신호를 받으면 새 손님을 막고 응답 중인 연결만 남긴다 (CON-15) */
static void begin_shutdown (server_t * server) {
    write_log(LOG_INFO, "종료 신호를 받음: 진행 중인 응답만 마저 보냄");
    server->is_shutting_down = 1;
    server->shutdown_deadline_ms = server->now_ms + SHUTDOWN_WAIT_MS;
    delete_event(server->loop, server->listen_fd);
    close(server->listen_fd);
    server->listen_fd = -1;
    server->is_accepting = 0;
    conn_t * conn = server->all_head;
    while (conn != NULL) {
        conn_t * next = conn->all_next;
        prepare_for_shutdown(server, conn);
        conn = next;
    }
}

static void prepare_for_shutdown (server_t * server, conn_t * conn) {
    if (conn->is_closed) {
        return;
    }
    if (conn->kind == CONN_CLIENT && conn->state == STATE_READING) {
        close_connection(server, conn);
        return;
    }
    if (conn->deadline_kind == DEADLINE_NONE
        || conn->deadline_ms > server->shutdown_deadline_ms) {
        set_connection_deadline(server, conn, DEADLINE_SHUTDOWN,
            SHUTDOWN_WAIT_MS);
    }
}

static int is_finished (const server_t * server) {
    return server->is_shutting_down && server->connection_count == 0;
}

/*
 * 최대 1초만 기다린다. 시그널이 플래그 확인과 대기 사이에 도착하면 영원히
 * 깨어나지 못할 수 있기 때문이다.
 */
static int compute_wait_timeout (const server_t * server) {
    if (server->pending_head != NULL) {
        return 0;
    }
    conn_t * next = find_earliest_conn(server->heap);
    if (next == NULL) {
        return WAIT_MAX_MS;
    }
    long long remaining = next->deadline_ms - get_now_ms();
    if (remaining <= 0) {
        return 0;
    }
    return remaining > WAIT_MAX_MS ? WAIT_MAX_MS : (int) remaining;
}

/* 이번 바퀴 시작 때 줄 서 있던 만큼만 처리한다. 새로 생긴 것은 다음 바퀴다 */
static void process_pending_requests (server_t * server) {
    size_t remaining = server->pending_count;
    while (remaining > 0 && server->pending_head != NULL) {
        conn_t * conn = server->pending_head;
        unlink_pending(server, conn);
        remaining--;
        process_buffered_request(server, conn);
    }
}

static void unlink_pending (server_t * server, conn_t * conn) {
    if (!conn->is_pending) {
        return;
    }
    if (conn->pending_previous != NULL) {
        conn->pending_previous->pending_next = conn->pending_next;
    } else {
        server->pending_head = conn->pending_next;
    }
    if (conn->pending_next != NULL) {
        conn->pending_next->pending_previous = conn->pending_previous;
    } else {
        server->pending_tail = conn->pending_previous;
    }
    conn->is_pending = 0;
    server->pending_count--;
}

static void process_buffered_request (server_t * server, conn_t * conn) {
    if (conn->state != STATE_READING || conn->input.length == 0) {
        return;
    }
    http_request_t request;
    int status_code = 0;
    parse_result_t result = parse_request(conn->input.data, conn->input.length,
        &request, &status_code);
    if (result == PARSE_ERROR) {
        reject_request(server, conn, status_code);
        return;
    }
    if (result == PARSE_NEED_MORE) {
        wait_for_more_input(server, conn, &request);
        return;
    }
    handle_request(server, conn, &request);
}

/* 경계를 믿을 수 없는 오류 뒤에는 연결을 닫는다 (REQ-21) */
static void reject_request (server_t * server, conn_t * conn,
    int status_code) {
    conn->keep_alive = 0;
    conn->is_head = 0;
    if (status_code == 503) {
        release_buffer(&conn->input);
    }
    queue_error_response(server, conn, status_code, NULL);
}

static void wait_for_more_input (server_t * server, conn_t * conn,
    const http_request_t * request) {
    if (conn->is_half_closed) {
        close_connection(server, conn);
        return;
    }
    if (!request->has_head || !request->expect_continue
        || conn->has_sent_continue) {
        return;
    }
    if (append_continue_response(&conn->output) != 0) {
        close_connection(server, conn);
        return;
    }
    conn->has_sent_continue = 1;
    update_interest(server, conn);
}

static void handle_request (server_t * server, conn_t * conn,
    const http_request_t * request) {
    conn->keep_alive = request->keep_alive && !server->is_shutting_down;
    conn->is_head = request->method == METHOD_HEAD;
    conn->is_http10 = request->version_minor == 0;
    route_t * route = find_route(server->proxy_config, request->path);
    if (route != NULL) {
        start_proxy_request(server, conn, request, route);
    } else if (request->method == METHOD_POST) {
        queue_error_response(server, conn, 405, "Allow: GET, HEAD\r\n");
    } else {
        start_static_response(server, conn, request);
    }
    if (!conn->is_closed) {
        consume_bytes(&conn->input, request->total_length);
    }
}

static void start_static_response (server_t * server, conn_t * conn,
    const http_request_t * request) {
    static_file_t file;
    int status_code = open_static_file(server->root_fd,
        server->root_path_length, request->path, &file);
    if (status_code == 200) {
        queue_file_response(server, conn, &file);
        return;
    }
    if (status_code == 301) {
        queue_redirect_response(server, conn, request);
        return;
    }
    queue_error_response(server, conn, status_code, NULL);
}

static void queue_file_response (server_t * server, conn_t * conn,
    const static_file_t * file) {
    if (append_response_header(&conn->output, 200, file->mime_type,
            (size_t) file->size, conn->keep_alive, NULL) != 0) {
        close(file->fd);
        close_connection(server, conn);
        return;
    }
    if (conn->is_head || file->size == 0) {
        close(file->fd);
        start_sending(server, conn);
        return;
    }
    conn->file_fd = file->fd;
    conn->file_remaining = file->size;
    if (fill_output_from_file(conn) != 0) {
        discard_file_response(server, conn);
        return;
    }
    start_sending(server, conn);
}

static void start_sending (server_t * server, conn_t * conn) {
    conn->state = STATE_WRITING;
    conn->is_response_done = 1;
    clear_connection_deadline(server, conn);
    update_interest(server, conn);
}

/* 한 번에 64KB까지만 읽는다. 파일 전체를 메모리에 올리지 않는다 (RES-08) */
static int fill_output_from_file (conn_t * conn) {
    size_t chunk = FILE_CHUNK_BYTES;
    if ((off_t) chunk > conn->file_remaining) {
        chunk = (size_t) conn->file_remaining;
    }
    if (reserve_buffer(&conn->output, chunk) != 0) {
        return -1;
    }
    ssize_t received;
    do {
        received = read(conn->file_fd, conn->output.data + conn->output.length,
            chunk);
    } while (received < 0 && errno == EINTR);
    if (received <= 0) {
        return -1;
    }
    conn->output.length += (size_t) received;
    conn->file_remaining -= received;
    if (conn->file_remaining == 0) {
        close(conn->file_fd);
        conn->file_fd = -1;
    }
    return 0;
}

/*
 * 읽기가 한 번도 성공하지 못했다면 아직 아무것도 보내지 않았으므로 500으로
 * 정상 응답할 수 있다 (RES-11).
 */
static void discard_file_response (server_t * server, conn_t * conn) {
    close(conn->file_fd);
    conn->file_fd = -1;
    conn->file_remaining = 0;
    consume_bytes(&conn->output, conn->output.length);
    queue_error_response(server, conn, 500, NULL);
}

static void queue_redirect_response (server_t * server, conn_t * conn,
    const http_request_t * request) {
    const char * uri = conn->input.data + request->uri_offset;
    size_t path_length = request->raw_path_length;
    size_t uri_length = request->uri_length;
    /* //로 시작하면 브라우저가 다른 사이트로 해석하므로 /를 하나만 남긴다 */
    while (path_length > 1 && uri[0] == '/' && uri[1] == '/') {
        uri++;
        path_length--;
        uri_length--;
    }
    char * header = create_location_header(uri, path_length, uri_length);
    if (header == NULL) {
        queue_error_response(server, conn, 500, NULL);
        return;
    }
    queue_error_response(server, conn, 301, header);
    free(header);
}

/* 새 메모리를 돌려준다. 받은 쪽이 free한다. 실패하면 NULL */
static char * create_location_header (const char * uri, size_t path_length,
    size_t uri_length) {
    const char * format = "Location: %.*s/%.*s\r\n";
    int tail_length = (int) (uri_length - path_length);
    int needed = snprintf(NULL, 0, format, (int) path_length, uri,
        tail_length, uri + path_length);
    if (needed < 0) {
        return NULL;
    }
    char * header = malloc((size_t) needed + 1);
    if (header == NULL) {
        return NULL;
    }
    snprintf(header, (size_t) needed + 1, format, (int) path_length, uri,
        tail_length, uri + path_length);
    return header;
}

static void dispatch_event (server_t * server, const event_t * event) {
    if (event->data == server) {
        if (event->readable) {
            accept_connections(server);
        }
        return;
    }
    conn_t * conn = event->data;
    if (conn->is_closed) {
        return;
    }
    if (conn->kind == CONN_BACKEND) {
        handle_backend_event(server, conn, event);
        return;
    }
    handle_client_event(server, conn, event);
}

static void accept_connections (server_t * server) {
    //한라운드에 최대 64명(ACCEPT_BATCH_MAX) 까지만 받음
    for (int i = 0; i < ACCEPT_BATCH_MAX; i++) {
        //지금 연결수가 상한 1만에 도달했으면 
        if (server->connection_count >= server->max_connections) {
            // 리슨 소켓 알림끔->새 손님왔다고 더는 알림 안옴
            pause_accepting(server);//받기 중단
            return;
        }
        //상한 1만 도달 안했으면  (accept → 구조체 만들기 → 15초 마감 → 이벤트 등록) 실패가너가 대기줄 비었으면(EAGAIN) false->이번 라운드 끝
        if (!accept_one_connection(server)) {
            return;
        }
    }
}

/* 상한에 닿으면 listen을 감시 목록에서 빼서 새 손님을 커널 줄에 둔다 */
static void pause_accepting (server_t * server) {
    if (!server->is_accepting) {
        return;
    }
    delete_event(server->loop, server->listen_fd);
    server->is_accepting = 0;
    write_log(LOG_INFO, "최대 연결 수에 도달: accept 중지");
}

/* 계속 받아도 되면 1, 이번 바퀴의 accept를 멈춰야 하면 0 */
static int accept_one_connection (server_t * server) {
    struct sockaddr_storage address;
    socklen_t address_length = sizeof(address);
    int fd = accept(server->listen_fd, (struct sockaddr *) &address,
        &address_length);
    if (fd < 0) {
        return handle_accept_failure(server);
    }
    register_client(server, fd, &address);
    return 1;
}
 
static int handle_accept_failure (server_t * server) {
    //write_log 가 아래에서 같은 다른 함수 부르면 errno바뀔수 있음 
    int error_number = errno;
    //줄에 기다리는 손님 없음(논블락킹 기다리지않고 바로 들어옴) 정상상황 라운드 끝
    if (error_number == EAGAIN || error_number == EWOULDBLOCK) {
        return 0;
    }
    //fd가 바닥남 EMFILE= 우리 프로세스 한도 꽉참 /ENFILE 컴퓨터 전체 한도 꽉참 비상용 fd 트릭으로 맨 줄 앞 손님 받자마자 끊음
    if (error_number == EMFILE || error_number == ENFILE) {
        discard_waiting_connection(server);//비상용 fd 내용은 이 함수안에 있음
        return 0;
    }
    write_log(LOG_DEBUG, "accept 실패: %s", strerror(error_number));
    //그밖의 실패 (예 줄에서 기다리던 손님이 연결 끊고 먼저 가버림)->그손님 하나만 문제니까 기록하고
    return 1;//다음 손님 계속 받아도 됨
}

/*
 * fd가 바닥나면 줄 서 있는 손님 때문에 listen이 계속 "준비됨"으로 떠서
 * 루프가 CPU 100%로 돈다. 비상용 fd를 닫아 그 자리에서 받아 버린 뒤
 * 다시 확보한다 (CON-11).
 */
static void discard_waiting_connection (server_t * server) {
    close(server->reserve_fd);
    int fd = accept(server->listen_fd, NULL, NULL);
    if (fd >= 0) {
        close(fd);
    }
    server->reserve_fd = open(RESERVE_PATH, O_RDONLY);
    write_log(LOG_INFO, "fd 한도에 도달해 대기 중인 손님을 거절함");
}

/* fd의 주인은 성공하면 conn이고, 실패하면 이 함수가 닫는다 */
static void register_client (server_t * server, int fd,
    const struct sockaddr_storage * address) {
    conn_t * conn = NULL;
    if (set_nonblocking(fd) == 0) {
        conn = create_connection(fd, CONN_CLIENT);
    }
    if (conn == NULL) {
        close(fd);
        return;
    }
    remember_client_address(conn, address);
    conn->state = STATE_READING;
    conn->wants_read = 1;
    if (register_connection(server, conn) != 0) {
        destroy_connection(conn);
        return;
    }
    server->connection_count++;
    /*
     * 정의서에는 첫 요청 전의 대기 시간이 없다. 영원히 붙잡히지 않도록 요청
     * 읽기 타임아웃과 같은 값을 안전장치로 둔다.
     */
    set_connection_deadline(server, conn, DEADLINE_IDLE, REQUEST_TIMEOUT_MS);
}

static void remember_client_address (conn_t * conn,
    const struct sockaddr_storage * address) {
    const void * source = NULL;
    if (address->ss_family == AF_INET) {
        source = &((const struct sockaddr_in *) address)->sin_addr;
    } else if (address->ss_family == AF_INET6) {
        source = &((const struct sockaddr_in6 *) address)->sin6_addr;
    }
    if (source == NULL || inet_ntop(address->ss_family, source,
            conn->client_address, sizeof(conn->client_address)) == NULL) {
        snprintf(conn->client_address, sizeof(conn->client_address),
            "unknown");
    }
}

static void handle_client_event (server_t * server, conn_t * conn,
    const event_t * event) {
    if (event->failed) {
        write_log(LOG_DEBUG, "손님 연결 오류로 정리 (fd=%d)", conn->fd);
        close_connection(server, conn);
        return;
    }
    if (event->readable) {
        read_from_client(server, conn);
    }
    if (event->writable && !conn->is_closed) {
        write_to_client(server, conn);
    }
}

/* 읽기 알림 한 번에 read는 한 번만 부르고 그 자리에서 기다리지 않는다 */
static void read_from_client (server_t * server, conn_t * conn) {
    if (conn->state != STATE_READING
        && conn->input.length >= PIPELINE_BYTES_MAX) {
        update_interest(server, conn);
        return;
    }
    int status_code = ensure_input_space(conn);
    if (status_code != 0) {
        handle_input_shortage(server, conn, status_code);
        return;
    }
    size_t room = conn->input.capacity - conn->input.length;
    ssize_t received = read(conn->fd, conn->input.data + conn->input.length,
        room);
    if (received > 0) {
        conn->input.length += (size_t) received;
        handle_client_data(server, conn);
    } else if (received == 0) {
        handle_client_eof(server, conn);
    } else {
        handle_read_error(server, conn);
    }
}

/* 0이면 읽을 자리가 있다. 아니면 응답할 상태 코드(413 또는 503) */
static int ensure_input_space (conn_t * conn) {
    if (conn->input.capacity > conn->input.length) {
        return 0;
    }
    if (conn->input.length >= INPUT_BUFFER_MAX) {
        return 413;
    }
    if (reserve_buffer(&conn->input, 1) != 0) {
        return 503;
    }
    return 0;
}

static void handle_input_shortage (server_t * server, conn_t * conn,
    int status_code) {
    if (conn->state == STATE_READING) {
        reject_request(server, conn, status_code);
        return;
    }
    close_connection(server, conn);
}

static void handle_client_data (server_t * server, conn_t * conn) {
    if (conn->state != STATE_READING) {
        update_interest(server, conn);
        return;
    }
    /* 첫 바이트가 올 때만 시작하고 이후 데이터가 와도 갱신하지 않는다 */
    if (conn->deadline_kind == DEADLINE_IDLE) {
        set_connection_deadline(server, conn, DEADLINE_REQUEST,
            REQUEST_TIMEOUT_MS);
    }
    process_buffered_request(server, conn);
}

/*
 * 반쪽 닫기: 더 받지 않지만 이미 받은 요청의 응답은 마저 보낸다 (NET-07).
 * 아직 처리할 요청이 없다면 바로 정리한다.
 */
static void handle_client_eof (server_t * server, conn_t * conn) {
    conn->is_half_closed = 1;
    if (conn->state == STATE_READING && !conn->is_pending) {
        write_log(LOG_DEBUG, "손님이 연결을 닫음 (fd=%d)", conn->fd);
        close_connection(server, conn);
        return;
    }
    update_interest(server, conn);
}

/* EINTR은 레벨 트리거라서 다음 알림에 자동으로 다시 시도된다 */
static void handle_read_error (server_t * server, conn_t * conn) {
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
        return;
    }
    write_log(LOG_DEBUG, "read 실패로 정리: %s", strerror(errno));
    close_connection(server, conn);
}

static void write_to_client (server_t * server, conn_t * conn) {
    if (conn->output.length == 0 && conn->file_remaining > 0
        && fill_output_from_file(conn) != 0) {
        write_log(LOG_INFO, "파일 읽기 실패로 연결을 닫음");
        close_connection(server, conn);
        return;
    }
    if (conn->output.length == 0) {
        update_after_write(server, conn);
        return;
    }
    ssize_t sent = write(conn->fd, conn->output.data, conn->output.length);
    if (sent < 0) {
        handle_write_error(server, conn);
        return;
    }
    consume_bytes(&conn->output, (size_t) sent);
    update_after_write(server, conn);
}

static void update_after_write (server_t * server, conn_t * conn) {
    if (conn->state == STATE_PROXYING && conn->peer != NULL) {
        update_interest(server, conn->peer);
    }
    if (conn->output.length == 0 && conn->file_remaining == 0
        && conn->is_response_done) {
        finish_response(server, conn);
        return;
    }
    update_interest(server, conn);
}

/* 손님이 먼저 끊은 것(EPIPE, ECONNRESET)은 그 연결만 정리한다 (NET-06) */
static void handle_write_error (server_t * server, conn_t * conn) {
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
        return;
    }
    write_log(LOG_DEBUG, "write 실패로 정리: %s", strerror(errno));
    close_connection(server, conn);
}

/* 마감이 가장 빠른 연결부터 지난 것을 차례로 정리한다 (CON-12) */
static void expire_timers (server_t * server) {
    for (;;) {
        conn_t * conn = find_earliest_conn(server->heap);
        if (conn == NULL || conn->deadline_ms > server->now_ms) {
            return;
        }
        handle_timeout(server, conn);
    }
}

/* 처리하기 전에 힙에서 먼저 빼야 같은 마감이 반복해서 걸리지 않는다 */
static void handle_timeout (server_t * server, conn_t * conn) {
    deadline_kind_t kind = conn->deadline_kind;
    remove_from_heap(server->heap, conn);
    conn->deadline_kind = DEADLINE_NONE;
    if (kind == DEADLINE_BACKEND_CONNECT
        || kind == DEADLINE_BACKEND_RESPONSE) {
        handle_backend_timeout(server, conn, kind);
        return;
    }
    write_log(LOG_INFO, "타임아웃으로 연결을 정리함 (종류 %d)", (int) kind);
    close_connection(server, conn);
}

static void resume_accepting (server_t * server) {
    if (server->is_accepting || server->is_shutting_down
        || server->connection_count >= server->max_connections) {
        return;
    }
    if (add_event(server->loop, server->listen_fd, server, 1, 0) != 0) {//0이 아니다 등록실패 이미 받는 중이면 아무것도 안 함
        write_log(LOG_ERROR, "accept 재개 실패: %s", strerror(errno));//에러 로그 남기고 종료
        return;
    }
    server->is_accepting = 1;//지금 손님 받는중 깃발세움
}

static void free_closed_connections (server_t * server) {
    while (server->closed_head != NULL) {
        conn_t * conn = server->closed_head;
        server->closed_head = conn->closed_next;
        destroy_connection(conn);
    }
}

static void link_connection (server_t * server, conn_t * conn) {
    conn->all_previous = NULL;
    conn->all_next = server->all_head;
    if (server->all_head != NULL) {
        server->all_head->all_previous = conn;
    }
    server->all_head = conn;
}

/* 짝을 먼저 끊어서 서로를 다시 부르며 무한히 돌지 않게 한다 (PRX-16) */
static void close_peer (server_t * server, conn_t * conn) {
    conn_t * peer = conn->peer;
    if (peer == NULL) {
        return;
    }
    conn->peer = NULL;
    peer->peer = NULL;
    close_connection(server, peer);
}

/*
 * 읽지 않은 데이터가 남은 채 close하면 RST가 나가서, 방금 보낸 오류 응답이
 * 손님에게 닿기 전에 사라질 수 있다. 이미 도착한 만큼만 버리고 끝낸다.
 */
static void discard_unread_bytes (int fd) {
    char sink[DISCARD_CHUNK_BYTES];
    for (int i = 0; i < DISCARD_ROUNDS_MAX; i++) {
        if (read(fd, sink, sizeof(sink)) <= 0) {
            return;
        }
    }
}

static void unlink_connection (server_t * server, conn_t * conn) {
    if (conn->all_previous != NULL) {
        conn->all_previous->all_next = conn->all_next;
    } else if (server->all_head == conn) {
        server->all_head = conn->all_next;
    }
    if (conn->all_next != NULL) {
        conn->all_next->all_previous = conn->all_previous;
    }
}

static void compute_interest (const conn_t * conn, int * wants_read,
    int * wants_write) {
    if (conn->kind == CONN_BACKEND) {
        compute_backend_interest(conn, wants_read, wants_write);
        return;
    }
    *wants_write = conn->output.length > 0 || conn->file_remaining > 0;
    *wants_read = !conn->is_half_closed
        && (conn->state == STATE_READING
            || conn->input.length < PIPELINE_BYTES_MAX);
}

static void reset_request_state (conn_t * conn) {
    conn->is_response_done = 0;
    conn->has_response_started = 0;
    conn->has_sent_continue = 0;
    conn->is_head = 0;
    conn->route = NULL;
    release_buffer(&conn->forward);
}

static void queue_pending (server_t * server, conn_t * conn) {
    if (conn->is_pending) {
        return;
    }
    conn->is_pending = 1;
    conn->pending_next = NULL;
    conn->pending_previous = server->pending_tail;
    if (server->pending_tail != NULL) {
        server->pending_tail->pending_next = conn;
    } else {
        server->pending_head = conn;
    }
    server->pending_tail = conn;
    server->pending_count++;
}
