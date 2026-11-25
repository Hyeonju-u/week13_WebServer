#include "event_loop.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/resource.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <unistd.h>

#include "backend_conn.h"
#include "connection.h"

/* (L8) 3단계는 루프 1개 */
#define LOOP_COUNT 1
/* (L8, E1) 고정 fd 6개(0·1·2, listen, epoll, signalfd)
 * + 가끔 쓰는 것(/etc/localtime, ASan) */
#define FD_RESERVE_BASE 16
/* (L8) 5단계에서 루프마다 epoll·eventfd */
#define FD_PER_LOOP 2
/* (L8, G19, G24) 손님 소켓 + 보내는 동안 연 파일 또는 백엔드 소켓.
 * 재시도는 앞 백엔드 소켓을 닫은 뒤 새로 열어서 3개가 되지 않는다 */
#define FD_PER_CONNECTION 2
/* (L23) */
#define LISTEN_RETRY_MS 100
/* (L43) epoll_wait 한 번에 받는 이벤트 수. nginx 기본값, 7단계에서 측정 */
#define MAX_EVENTS_PER_WAIT 512
/* (L29, G30) 카드가 들어가는 타이머 리스트 수. 살아 있는 카드는 언제나
 * 이 중 하나에만 있어서, 종료 때 카드를 찾는 목록으로도 쓴다 (E4, E5) */
#define CONN_TIMER_LIST_COUNT 8

static bool find_max_connections (size_t * max_connections,
                                  unsigned long long * soft_limit);
static void report_error (const char * what);
static int create_signal_fd (void);
static void set_timer_durations (loop_t * loop);
static bool add_listen (loop_t * loop);
static bool add_signal (loop_t * loop);
static int find_wait_ms (loop_t * loop);
static void fill_conn_lists (loop_t * loop,
                             timer_list_t * lists[CONN_TIMER_LIST_COUNT]);
static long long pick_earlier (long long current, long long candidate);
static void handle_events (loop_t * loop, struct epoll_event * events,
                           int count);
static void handle_signal (loop_t * loop);
static void start_shutdown (loop_t * loop);
static void close_listen (loop_t * loop);
static void remove_listen (loop_t * loop);
static void shut_down_list (loop_t * loop, timer_list_t * list);
static void force_shutdown (loop_t * loop);
static size_t destroy_all_conns (loop_t * loop);
static void finish_shutdown (loop_t * loop);
static conn_t * find_event_conn (void * pointer);
static void dispatch_event (loop_t * loop, conn_t ** slot, void * pointer,
                            uint32_t events);
static void accept_connection (loop_t * loop);
static void handle_accept_error (loop_t * loop);
static bool is_ignorable_accept_error (int error);
static void expire_timers (loop_t * loop);
static void expire_list (loop_t * loop, timer_list_t * list, long long now);
static void retry_listen (loop_t * loop);
static void check_shutdown_done (loop_t * loop);
static bool prepare_client_socket (int fd);

/* ── 공개 함수 ── */

/* (L1, L8, L22, E1) 실패할 수 있는 일(한도 읽기, 할당, epoll, signalfd)을
 * 먼저 끝낸다 */
loop_t * create_loop (const server_config_t * config, upstream_t * upstream,
                      int listen_fd) {
    size_t max_connections = 0;
    unsigned long long soft_limit = 0;

    if (!find_max_connections(&max_connections, &soft_limit)) {
        return NULL;
    }
    loop_t * loop = malloc(sizeof(*loop));
    if (loop == NULL) {
        report_error("loop");
        return NULL;
    }
    memset(loop, 0, sizeof(*loop));
    loop->epoll_fd = epoll_create1(0);
    loop->signal_fd = create_signal_fd();
    if (loop->epoll_fd < 0 || loop->signal_fd < 0) {
        report_error(loop->epoll_fd < 0 ? "epoll_create1" : "signalfd");
        close(loop->epoll_fd);
        close(loop->signal_fd);
        free(loop);
        return NULL;
    }
    loop->config = config;
    loop->upstream = upstream;
    loop->listen_fd = listen_fd;
    loop->max_connections = max_connections;
    loop->retry_timer.owner = loop;
    loop->shutdown_timer.owner = loop;
    set_timer_durations(loop);
    if (!add_listen(loop) || !add_signal(loop)) {
        close(loop->epoll_fd);
        close(loop->signal_fd);
        free(loop);
        return NULL;
    }
    fprintf(stderr, "max connections: %zu (nofile soft limit %llu)\n",
            max_connections, soft_limit);
    return loop;
}

/* (R1a, L3, L29, G25, E4, E5) 이벤트 → 마감된 타이머 → 정리한 카드 free →
 * 종료 중이면 카드가 다 사라졌는지 확인. free를 묶음 끝에 해야 같은 묶음의
 * 다른 이벤트가 카드를 볼 수 있다 */
bool run_loop (loop_t * loop) {
    struct epoll_event events[MAX_EVENTS_PER_WAIT];

    while (!loop->is_finished) {
        int count = epoll_wait(loop->epoll_fd, events, MAX_EVENTS_PER_WAIT,
                               find_wait_ms(loop));
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count < 0) {
            report_error("epoll_wait");
            return false;
        }
        handle_events(loop, events, count);
        expire_timers(loop);
        free_closed_conns(loop);
        check_shutdown_done(loop);
    }
    return true;
}

/* (E5, G25) 정상 종료면 카드가 이미 없다. epoll_wait 실패로 돌아온 경우에도
 * 같은 길로 정리한다 */
void destroy_loop (loop_t * loop) {
    destroy_all_conns(loop);
    free_closed_conns(loop);
    if (loop->listen_fd >= 0) {
        close(loop->listen_fd);
    }
    close(loop->signal_fd);
    close(loop->epoll_fd);
    free(loop);
}

/* (L1, L8, L28, L31) 한도에 닿으면 listen을 빼서 손님은 커널 대기열에서 기다린다 */
void register_connection (loop_t * loop, int fd,
                          const struct sockaddr_in * peer) {
    if (!prepare_client_socket(fd)) {
        report_error("client socket");
        close(fd);
        return;
    }
    conn_t * conn = create_conn(fd, peer);
    if (conn == NULL) {
        report_error("connection");
        close(fd);
        return;
    }
    loop->created_count++;
    loop->connection_count++;
    if (!start_conn(loop, conn)) {
        report_error("epoll_ctl");
        destroy_conn(loop, &conn, NULL);
        return;
    }
    if (loop->connection_count >= loop->max_connections) {
        remove_listen(loop);
    }
}

/* (L8, L23, E3) 자리가 나서 먼저 다시 넣으면 재시도 타이머는 취소한다.
 * 다시 넣기가 실패하면 마지막 연결이었을 때 영영 못 받으므로 재시도.
 * 종료 중에는 listen이 닫혀 있어 다시 넣지 않는다 */
void release_connection_slot (loop_t * loop) {
    loop->connection_count--;
    if (loop->is_shutting_down || loop->is_listening
        || loop->connection_count >= loop->max_connections) {
        return;
    }
    stop_timer(&loop->retry_timer);
    if (!add_listen(loop)) {
        start_timer(&loop->retry_timers, &loop->retry_timer, read_clock_ms());
    }
}

/* ── 도우미 함수 ── */

/* (L8, L22) 최대 연결 = (소프트 한도 − 여유분) ÷ 2.
 * 반환: false = 한도를 못 읽었거나 연결 하나도 못 받는 한도 (stderr에 이유) */
static bool find_max_connections (size_t * max_connections,
                                  unsigned long long * soft_limit) {
    struct rlimit limit;
    unsigned long long reserve = FD_RESERVE_BASE + LOOP_COUNT * FD_PER_LOOP;

    if (getrlimit(RLIMIT_NOFILE, &limit) != 0) {
        report_error("getrlimit");
        return false;
    }
    *soft_limit = (unsigned long long) limit.rlim_cur;
    if (*soft_limit < reserve + FD_PER_CONNECTION) {
        fprintf(stderr, "nofile soft limit %llu is too small\n", *soft_limit);
        return false;
    }
    *max_connections = (size_t) ((*soft_limit - reserve) / FD_PER_CONNECTION);
    return true;
}

/* (L15) 내부 오류는 stderr */
static void report_error (const char * what) {
    fprintf(stderr, "%s: %s\n", what, strerror(errno));
}

/* (E1, E2) SIGINT·SIGTERM을 막아 두고 signalfd로 읽는다.
 * 막은 뒤에 온 신호는 signalfd에 쌓여 epoll이 알려 준다. 반환: -1 = 실패 */
static int create_signal_fd (void) {
    sigset_t signals;

    sigemptyset(&signals);
    sigaddset(&signals, SIGINT);
    sigaddset(&signals, SIGTERM);
    if (sigprocmask(SIG_BLOCK, &signals, NULL) != 0) {
        return -1;
    }
    return signalfd(-1, &signals, SFD_NONBLOCK | SFD_CLOEXEC);
}

/* (L5, L11, L12, L23, L29, G11, G30, E5) */
static void set_timer_durations (loop_t * loop) {
    loop->header_timers.duration_ms = loop->config->header_timeout_ms;
    loop->send_timers.duration_ms = loop->config->send_timeout_ms;
    loop->idle_timers.duration_ms = loop->config->keepalive_timeout_ms;
    loop->quiet_timers.duration_ms = loop->config->linger_quiet_ms;
    loop->body_timers.duration_ms = loop->config->body_timeout_ms;
    loop->connect_timers.duration_ms =
        loop->config->backend_connect_timeout_ms;
    loop->backend_send_timers.duration_ms =
        loop->config->backend_send_timeout_ms;
    loop->backend_read_timers.duration_ms =
        loop->config->backend_read_timeout_ms;
    loop->retry_timers.duration_ms = LISTEN_RETRY_MS;
    loop->shutdown_timers.duration_ms = loop->config->shutdown_timeout_ms;
}

/* (L1, L8) data.ptr == NULL이 listen 소켓이라는 표시다 */
static bool add_listen (loop_t * loop) {
    struct epoll_event event = { .events = EPOLLIN, .data.ptr = NULL };

    if (epoll_ctl(loop->epoll_fd, EPOLL_CTL_ADD, loop->listen_fd,
                  &event) != 0) {
        report_error("epoll_ctl listen");
        return false;
    }
    loop->is_listening = true;
    return true;
}

/* (E1) data.ptr == &loop->signal_fd가 signalfd라는 표시다 */
static bool add_signal (loop_t * loop) {
    struct epoll_event event = { .events = EPOLLIN,
                                 .data.ptr = &loop->signal_fd };

    if (epoll_ctl(loop->epoll_fd, EPOLL_CTL_ADD, loop->signal_fd,
                  &event) != 0) {
        report_error("epoll_ctl signalfd");
        return false;
    }
    return true;
}

/* (L29, E5) 각 리스트 맨 앞 마감 중 가장 이른 것까지. 없으면 -1 (무한) */
static int find_wait_ms (loop_t * loop) {
    timer_list_t * lists[CONN_TIMER_LIST_COUNT];
    long long now = read_clock_ms();
    long long wait = -1;

    fill_conn_lists(loop, lists);
    for (size_t i = 0; i < CONN_TIMER_LIST_COUNT; i++) {
        wait = pick_earlier(wait, find_time_until_first(lists[i], now));
    }
    wait = pick_earlier(wait, find_time_until_first(&loop->retry_timers, now));
    wait = pick_earlier(wait,
                        find_time_until_first(&loop->shutdown_timers, now));
    return wait > INT_MAX ? INT_MAX : (int) wait;
}

/* (L29, G30) 카드가 들어가는 리스트. retry·shutdown은 주인이 루프라 뺀다 */
static void fill_conn_lists (loop_t * loop,
                             timer_list_t * lists[CONN_TIMER_LIST_COUNT]) {
    lists[0] = &loop->header_timers;
    lists[1] = &loop->send_timers;
    lists[2] = &loop->idle_timers;
    lists[3] = &loop->quiet_timers;
    lists[4] = &loop->body_timers;
    lists[5] = &loop->connect_timers;
    lists[6] = &loop->backend_send_timers;
    lists[7] = &loop->backend_read_timers;
}

/* (L29) -1은 "없음" */
static long long pick_earlier (long long current, long long candidate) {
    if (candidate < 0) {
        return current;
    }
    if (current < 0 || candidate < current) {
        return candidate;
    }
    return current;
}

/* (L1, L3, G25, E1) 카드 하나에 fd가 둘이라 앞 이벤트가 카드를 정리했을 수
 * 있다. 카드는 묶음이 끝날 때까지 free되지 않으므로 closed만 보고 건너뛴다 */
static void handle_events (loop_t * loop, struct epoll_event * events,
                           int count) {
    for (int i = 0; i < count; i++) {
        if (events[i].data.ptr == &loop->signal_fd) {
            handle_signal(loop);
            continue;
        }
        if (events[i].data.ptr == NULL) {
            accept_connection(loop);
            continue;
        }
        conn_t * conn = find_event_conn(events[i].data.ptr);
        if (conn->closed) {
            continue;
        }
        dispatch_event(loop, &conn, events[i].data.ptr, events[i].events);
    }
}

/* (E2) 첫 신호 = 정상 종료 시작, 종료 중 신호 = 마감을 지금으로 당김.
 * 한 번 깨어날 때 신호 하나 (L30) */
static void handle_signal (loop_t * loop) {
    struct signalfd_siginfo info;

    if (read(loop->signal_fd, &info, sizeof(info)) != sizeof(info)) {
        return;
    }
    if (!loop->is_shutting_down) {
        start_shutdown(loop);
        return;
    }
    force_shutdown(loop);
}

/* (E3, E4, E5, E7) listen 닫기 → 마감 타이머 → 카드마다 E4 처리.
 * 카드가 다 사라지면 묶음 끝의 check_shutdown_done이 끝낸다 */
static void start_shutdown (loop_t * loop) {
    timer_list_t * lists[CONN_TIMER_LIST_COUNT];

    fprintf(stderr, "shutdown: start (active %zu)\n",
            loop->connection_count);
    loop->is_shutting_down = true;
    close_listen(loop);
    start_timer(&loop->shutdown_timers, &loop->shutdown_timer,
                read_clock_ms());
    fill_conn_lists(loop, lists);
    for (size_t i = 0; i < CONN_TIMER_LIST_COUNT; i++) {
        shut_down_list(loop, lists[i]);
    }
}

/* (E3, L23) 새 connect는 커널이 거절한다. 재시도 타이머도 멈춘다 */
static void close_listen (loop_t * loop) {
    remove_listen(loop);
    stop_timer(&loop->retry_timer);
    close(loop->listen_fd);
    loop->listen_fd = -1;
    loop->is_listening = false;
}

/* (L8) */
static void remove_listen (loop_t * loop) {
    if (!loop->is_listening) {
        return;
    }
    if (epoll_ctl(loop->epoll_fd, EPOLL_CTL_DEL, loop->listen_fd,
                  NULL) != 0) {
        report_error("epoll_ctl listen");
        return;
    }
    loop->is_listening = false;
}

/* (E4) shut_down_conn은 그 카드만 리스트에서 뺄 수 있어서 다음을 먼저 잡는다 */
static void shut_down_list (loop_t * loop, timer_list_t * list) {
    timer_node_t * node = list->first;

    while (node != NULL) {
        timer_node_t * next = node->next;
        conn_t * conn = node->owner;
        shut_down_conn(loop, &conn);
        node = next;
    }
}

/* (E2, E5, E7) 마감이 됨 (또는 두 번째 신호): 남은 카드를 전부 정리한다 */
static void force_shutdown (loop_t * loop) {
    loop->forced_count += destroy_all_conns(loop);
    finish_shutdown(loop);
}

/* (E5, L28, G26) 정리는 destroy_conn 하나 (→ release_backend).
 * 반환: 정리한 카드 수 */
static size_t destroy_all_conns (loop_t * loop) {
    timer_list_t * lists[CONN_TIMER_LIST_COUNT];
    size_t count = 0;

    fill_conn_lists(loop, lists);
    for (size_t i = 0; i < CONN_TIMER_LIST_COUNT; i++) {
        while (lists[i]->first != NULL) {
            conn_t * conn = lists[i]->first->owner;
            destroy_conn(loop, &conn, "close");
            count++;
        }
    }
    return count;
}

/* (E5, E7) 두 번 불러도 한 번만 찍는다 */
static void finish_shutdown (loop_t * loop) {
    if (loop->is_finished) {
        return;
    }
    stop_timer(&loop->shutdown_timer);
    fprintf(stderr, "shutdown: done (forced %zu)\n", loop->forced_count);
    loop->is_finished = true;
}

/* (G25) data.ptr은 카드(손님 fd) 또는 &conn->proxy(백엔드 fd).
 * 둘 다 첫 필드가 종류 표시다 */
static conn_t * find_event_conn (void * pointer) {
    endpoint_kind_t kind = *(endpoint_kind_t *) pointer;

    if (kind == ENDPOINT_BACKEND) {
        return ((proxy_t *) pointer)->owner;
    }
    return pointer;
}

/* (G25) */
static void dispatch_event (loop_t * loop, conn_t ** slot, void * pointer,
                            uint32_t events) {
    if (*(endpoint_kind_t *) pointer == ENDPOINT_BACKEND) {
        handle_backend_event(loop, slot, events);
        return;
    }
    handle_conn_event(loop, slot, events);
}

/* (R1a, L1, L8, L30) 한 번 깨어날 때 accept 1개. 같은 묶음 안에서
 * listen을 이미 뺐으면(한도·종료) 받지 않는다 */
static void accept_connection (loop_t * loop) {
    struct sockaddr_in peer;
    socklen_t length = sizeof(peer);

    if (!loop->is_listening) {
        return;
    }
    int fd = accept(loop->listen_fd, (struct sockaddr *) &peer, &length);
    if (fd < 0) {
        handle_accept_error(loop);
        return;
    }
    register_connection(loop, fd, &peer);
}

/* (L3, L8, L23) EMFILE을 그대로 두면 level에서 무한 반복(CPU 100%) */
static void handle_accept_error (loop_t * loop) {
    if (errno == EMFILE || errno == ENFILE) {
        report_error("accept");
        remove_listen(loop);
        start_timer(&loop->retry_timers, &loop->retry_timer, read_clock_ms());
        return;
    }
    if (is_ignorable_accept_error(errno)) {
        return;
    }
    report_error("accept");
}

/* (R1a) 손님이 accept 전에 떠났거나 이미 다른 이벤트로 받아 감 */
static bool is_ignorable_accept_error (int error) {
    return error == EAGAIN || error == EWOULDBLOCK || error == EINTR
           || error == ECONNABORTED || error == EPROTO;
}

/* (L5, L23, L29, G30, E5) */
static void expire_timers (loop_t * loop) {
    timer_list_t * lists[CONN_TIMER_LIST_COUNT];
    long long now = read_clock_ms();

    fill_conn_lists(loop, lists);
    for (size_t i = 0; i < CONN_TIMER_LIST_COUNT; i++) {
        expire_list(loop, lists[i], now);
    }
    if (find_expired_timer(&loop->retry_timers, now) != NULL) {
        retry_listen(loop);
    }
    if (find_expired_timer(&loop->shutdown_timers, now) != NULL) {
        force_shutdown(loop);
    }
}

/* (L29) expire_conn은 노드를 이 리스트에서 빼거나(정리) 다른 리스트로 옮긴다 */
static void expire_list (loop_t * loop, timer_list_t * list, long long now) {
    timer_node_t * node = find_expired_timer(list, now);

    while (node != NULL) {
        conn_t * conn = node->owner;
        expire_conn(loop, &conn);
        node = find_expired_timer(list, now);
    }
}

/* (L23, E3) 아직 한도에 닿아 있으면 연결이 닫힐 때 넣는다.
 * 종료 중에는 넣지 않는다 (close_listen이 타이머를 멈추지만 한 번 더 막음) */
static void retry_listen (loop_t * loop) {
    stop_timer(&loop->retry_timer);
    if (loop->is_shutting_down
        || loop->connection_count >= loop->max_connections) {
        return;
    }
    if (!add_listen(loop)) {
        start_timer(&loop->retry_timers, &loop->retry_timer, read_clock_ms());
    }
}

/* (E4) 모든 카드가 사라지면 마감 전이라도 바로 끝낸다 */
static void check_shutdown_done (loop_t * loop) {
    if (loop->is_shutting_down && loop->connection_count == 0) {
        finish_shutdown(loop);
    }
}

/* (R3c, L31) */
static bool prepare_client_socket (int fd) {
    int flags = fcntl(fd, F_GETFL);
    int enabled = 1;

    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
        return false;
    }
    return setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &enabled,
                      sizeof(enabled)) == 0;
}
