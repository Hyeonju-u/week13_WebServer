#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "event_loop.h"
#include "server_config.h"
#include "upstream.h"

/* (L35, G31) 명령·설정을 잘못 씀 = 2, 환경이 잘못됨 = 1 */
#define EXIT_USAGE 2
#define EXIT_SETUP 1

static void prepare_process (void);
static int pick_exit_code (config_result_t result);
static int create_listen_socket (int port);
static bool configure_listen_socket (int fd, int port);

/* ── 공개 함수 ── */

/* (L17, L22, G15, G29, G31) ./server <설정 파일> */
int main (int argc, char ** argv) {
    server_config_t config;
    char error[CONFIG_ERROR_SIZE];

    prepare_process();
    if (argc != 2) {
        fprintf(stderr, "usage: %s <config file>\n", argv[0]);
        return EXIT_USAGE;
    }
    config_result_t result = load_server_config(argv[1], &config, error,
                                                sizeof(error));
    if (result != CONFIG_OK) {
        fprintf(stderr, "%s\n", error);
        return pick_exit_code(result);
    }
    upstream_t * upstream = create_upstream(&config);
    if (upstream == NULL) {
        fprintf(stderr, "upstream: %s\n", strerror(errno));
        destroy_server_config(&config);
        return EXIT_SETUP;
    }
    int listen_fd = create_listen_socket(config.port);
    if (listen_fd < 0) {
        destroy_upstream(upstream);
        destroy_server_config(&config);
        return EXIT_SETUP;
    }
    loop_t * loop = create_loop(&config, upstream, listen_fd);
    if (loop == NULL) {
        close(listen_fd);
        destroy_upstream(upstream);
        destroy_server_config(&config);
        return EXIT_SETUP;
    }
    run_loop(loop);
    return EXIT_SETUP;
}

/* ── 도우미 함수 ── */

/* (L15, L17) 끊긴 손님에게 쓰면 죽지 않게, 죽기 직전 줄이 사라지지 않게 */
static void prepare_process (void) {
    signal(SIGPIPE, SIG_IGN);
    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IOLBF, 0);
}

/* (G31) */
static int pick_exit_code (config_result_t result) {
    return result == CONFIG_INVALID ? EXIT_USAGE : EXIT_SETUP;
}

/* (R1a) 반환: listen 중인 non-blocking 소켓, -1 = 실패 (stderr에 이유) */
static int create_listen_socket (int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);

    if (fd < 0) {
        fprintf(stderr, "socket: %s\n", strerror(errno));
        return -1;
    }
    if (!configure_listen_socket(fd, port)) {
        fprintf(stderr, "listen on %d: %s\n", port, strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

/* (R1a, L3, L22, L38) SO_REUSEADDR → 0.0.0.0 bind → SOMAXCONN → non-blocking */
static bool configure_listen_socket (int fd, int port) {
    int enabled = 1;
    struct sockaddr_in address;

    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons((uint16_t) port);
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enabled,
                   sizeof(enabled)) != 0) {
        return false;
    }
    if (bind(fd, (struct sockaddr *) &address, sizeof(address)) != 0) {
        return false;
    }
    if (listen(fd, SOMAXCONN) != 0) {
        return false;
    }
    int flags = fcntl(fd, F_GETFL);
    return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}
