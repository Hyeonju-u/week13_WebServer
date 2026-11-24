#include "event_loop.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>

#include "connection.h"

/* (L8) 3단계는 루프 1개 */
#define LOOP_COUNT 1
/* (L8) 고정 fd 5개(0·1·2, listen, epoll) + 가끔 쓰는 것(/etc/localtime, ASan) */
#define FD_RESERVE_BASE 16
/* (L8) 5단계에서 루프마다 epoll·eventfd */
#define FD_PER_LOOP 2
/* (L8) 손님 소켓 + 보내는 동안 연 파일 (4단계는 백엔드 소켓) */
#define FD_PER_CONNECTION 2
/* (L23) */
#define LISTEN_RETRY_MS 100
/* epoll_wait 한 번에 받는 이벤트 수 (새 상수, 결과 보고 참고) */
#define MAX_EVENTS_PER_WAIT 64

static bool find_max_connections (size_t * max_connections,
                                  unsigned long long * soft_limit);
static void report_error (const char * what);
static void set_timer_durations (loop_t * loop);
static bool add_listen (loop_t * loop);
static int find_wait_ms (loop_t * loop);
static long long pick_earlier (long long current, long long candidate);
static void handle_events (loop_t * loop, struct epoll_event * events,
                           int count);
static void accept_connection (loop_t * loop);
static void handle_accept_error (loop_t * loop);
static void remove_listen (loop_t * loop);
static bool is_ignorable_accept_error (int error);
static void expire_timers (loop_t * loop);
static void expire_list (loop_t * loop, timer_list_t * list, long long now);
static void retry_listen (loop_t * loop);
static bool prepare_client_socket (int fd);

/* ── 공개 함수 ── */

/* (L1, L8, L22) 실패할 수 있는 일(한도 읽기, 할당, epoll)을 먼저 끝낸다 */
loop_t * create_loop (const server_config_t * config, int listen_fd) {
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
    if (loop->epoll_fd < 0) {
        report_error("epoll_create1");
        free(loop);
        return NULL;
    }
    loop->config = config;
    loop->listen_fd = listen_fd;
    loop->max_connections = max_connections;
    loop->retry_timer.owner = loop;
    set_timer_durations(loop);
    if (!add_listen(loop)) {
        close(loop->epoll_fd);
        free(loop);
        return NULL;
    }
    fprintf(stderr, "max connections: %zu (nofile soft limit %llu)\n",
            max_connections, soft_limit);
    return loop;
}

/* (R1a, L3, L29) 이벤트를 처리한 뒤 마감된 타이머를 처리한다 */
void run_loop (loop_t * loop) {
    struct epoll_event events[MAX_EVENTS_PER_WAIT];

    for (;;) {
        int count = epoll_wait(loop->epoll_fd, events, MAX_EVENTS_PER_WAIT,
                               find_wait_ms(loop));
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count < 0) {
            report_error("epoll_wait");
            return;
        }
        handle_events(loop, events, count);
        expire_timers(loop);
    }
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

/* (L8, L23) 자리가 나서 먼저 다시 넣으면 재시도 타이머는 취소한다 */
void release_connection_slot (loop_t * loop) {
    loop->connection_count--;
    if (loop->is_listening
        || loop->connection_count >= loop->max_connections) {
        return;
    }
    stop_timer(&loop->retry_timer);
    add_listen(loop);
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

/* (L5, L11, L12, L23, L29) */
static void set_timer_durations (loop_t * loop) {
    loop->header_timers.duration_ms = loop->config->header_timeout_ms;
    loop->send_timers.duration_ms = loop->config->send_timeout_ms;
    loop->idle_timers.duration_ms = loop->config->keepalive_timeout_ms;
    loop->quiet_timers.duration_ms = loop->config->linger_quiet_ms;
    loop->retry_timers.duration_ms = LISTEN_RETRY_MS;
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

/* (L29) 각 리스트 맨 앞 마감 중 가장 이른 것까지. 없으면 -1 (무한) */
static int find_wait_ms (loop_t * loop) {
    timer_list_t * lists[] = {
        &loop->header_timers, &loop->send_timers, &loop->idle_timers,
        &loop->quiet_timers, &loop->retry_timers,
    };
    long long now = read_clock_ms();
    long long wait = -1;

    for (size_t i = 0; i < sizeof(lists) / sizeof(lists[0]); i++) {
        wait = pick_earlier(wait, find_time_until_first(lists[i], now));
    }
    return wait > INT_MAX ? INT_MAX : (int) wait;
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

/* (L1, L3) 카드는 자기 이벤트에서만 정리되므로 같은 묶음의 다른 이벤트는 안전 */
static void handle_events (loop_t * loop, struct epoll_event * events,
                           int count) {
    for (int i = 0; i < count; i++) {
        if (events[i].data.ptr == NULL) {
            accept_connection(loop);
            continue;
        }
        conn_t * conn = events[i].data.ptr;
        handle_conn_event(loop, &conn, events[i].events);
    }
}

/* (R1a, L1, L8, L30) 한 번 깨어날 때 accept 1개. 같은 묶음 안에서
 * listen을 이미 뺐으면(한도) 받지 않는다 */
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

/* (R1a) 손님이 accept 전에 떠났거나 이미 다른 이벤트로 받아 감 */
static bool is_ignorable_accept_error (int error) {
    return error == EAGAIN || error == EWOULDBLOCK || error == EINTR
           || error == ECONNABORTED || error == EPROTO;
}

/* (L5, L23, L29) */
static void expire_timers (loop_t * loop) {
    long long now = read_clock_ms();

    expire_list(loop, &loop->header_timers, now);
    expire_list(loop, &loop->send_timers, now);
    expire_list(loop, &loop->idle_timers, now);
    expire_list(loop, &loop->quiet_timers, now);
    if (find_expired_timer(&loop->retry_timers, now) != NULL) {
        retry_listen(loop);
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

/* (L23) 아직 한도에 닿아 있으면 연결이 닫힐 때 넣는다 */
static void retry_listen (loop_t * loop) {
    stop_timer(&loop->retry_timer);
    if (loop->connection_count >= loop->max_connections) {
        return;
    }
    if (!add_listen(loop)) {
        start_timer(&loop->retry_timers, &loop->retry_timer, read_clock_ms());
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
