#include "signals.h"

#include <signal.h>
#include <stdatomic.h>
#include <string.h>

/* 루프 스레드 여러 개가 같이 읽으므로 원자 변수로 둔다. 잠금 없는 int라
 * 핸들러 안에서 써도 안전하다 (M6) */
static atomic_int shutdown_flag = 0;

static void handle_shutdown_signal (int signal_number);

/* ── 공개 함수 ── */

/*
 * SA_RESTART를 쓰지 않아야 이벤트 대기가 EINTR로 깨어나 종료 플래그를
 * 확인할 수 있다 (NET-04).
 */
int install_signal_handlers (void) {
    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
        return -1;
    }
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = handle_shutdown_signal;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;
    if (sigaction(SIGINT, &action, NULL) != 0) {
        return -1;
    }
    return sigaction(SIGTERM, &action, NULL);
}

int is_shutdown_requested (void) {
    return atomic_load(&shutdown_flag) != 0;
}

/* 루프 하나가 멈추거나 스레드를 못 만들면 나머지 루프도 같은 종료 절차를
 * 밟게 한다 (M6) */
void request_shutdown (void) {
    atomic_store(&shutdown_flag, 1);
}

/* ── 도우미 함수 ── */

/* 핸들러 안에서는 플래그 하나만 바꾼다. close·free·printf는 안전하지 않다 */
static void handle_shutdown_signal (int signal_number) {
    (void) signal_number;
    atomic_store(&shutdown_flag, 1);
}
