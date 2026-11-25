#include "signals.h"

#include <signal.h>
#include <string.h>

static volatile sig_atomic_t shutdown_flag = 0;

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
    return shutdown_flag != 0;
}

/* ── 도우미 함수 ── */

/* 핸들러 안에서는 플래그 하나만 바꾼다. close·free·printf는 안전하지 않다 */
static void handle_shutdown_signal (int signal_number) {
    (void) signal_number;
    shutdown_flag = 1;
}
