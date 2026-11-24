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
#include "static_file.h"

/* (L35) 명령을 잘못 침 = 2, 환경이 잘못됨 = 1 */
#define EXIT_USAGE 2
#define EXIT_SETUP 1

/* (L5, L11, L12) 기본값은 nginx 기본값 */
#define DEFAULT_HEADER_TIMEOUT_MS 60000
#define DEFAULT_SEND_TIMEOUT_MS 60000
#define DEFAULT_KEEPALIVE_TIMEOUT_MS 75000
#define DEFAULT_LINGER_QUIET_MS 5000
#define DEFAULT_LINGER_TOTAL_MS 30000

#define MIN_PORT 1
#define MAX_PORT 65535

typedef struct {
    const char * prefix;
    long long * value;
} timeout_option_t;

static void prepare_process (void);
static void set_default_config (server_config_t * config);
static bool parse_arguments (int argc, char ** argv, int * port,
                             server_config_t * config);
static bool parse_port (const char * text, int * port);
static bool is_all_digits (const char * text);
static bool parse_option (const char * text, server_config_t * config);
static bool parse_timeout_option (const char * text,
                                  server_config_t * config);
static bool parse_milliseconds (const char * text, long long * value);
static void print_usage (const char * program);
static int create_listen_socket (int port);
static bool configure_listen_socket (int fd, int port);

/* ── 공개 함수 ── */

/* (L2, L17, L22, L35) */
int main (int argc, char ** argv) {
    server_config_t config;
    int port = 0;

    prepare_process();
    set_default_config(&config);
    if (!parse_arguments(argc, argv, &port, &config)) {
        print_usage(argv[0]);
        return EXIT_USAGE;
    }
    char * root = copy_static_root(argv[2]);
    if (root == NULL) {
        fprintf(stderr, "static root %s: %s\n", argv[2], strerror(errno));
        return EXIT_SETUP;
    }
    config.root = root;
    int listen_fd = create_listen_socket(port);
    if (listen_fd < 0) {
        free(root);
        return EXIT_SETUP;
    }
    loop_t * loop = create_loop(&config, listen_fd);
    if (loop == NULL) {
        close(listen_fd);
        free(root);
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

/* (L2, L5, L15, L18, L35, L36) */
static void set_default_config (server_config_t * config) {
    config->root = NULL;
    config->access_log = true;
    config->count_log = false;
    config->header_timeout_ms = DEFAULT_HEADER_TIMEOUT_MS;
    config->send_timeout_ms = DEFAULT_SEND_TIMEOUT_MS;
    config->keepalive_timeout_ms = DEFAULT_KEEPALIVE_TIMEOUT_MS;
    config->linger_quiet_ms = DEFAULT_LINGER_QUIET_MS;
    config->linger_total_ms = DEFAULT_LINGER_TOTAL_MS;
}

/* (L2) ./server <포트> <정적 루트> [옵션...] */
static bool parse_arguments (int argc, char ** argv, int * port,
                             server_config_t * config) {
    if (argc < 3) {
        return false;
    }
    if (!parse_port(argv[1], port)) {
        return false;
    }
    for (int i = 3; i < argc; i++) {
        if (!parse_option(argv[i], config)) {
            return false;
        }
    }
    return true;
}

/* (L35) 숫자만, 1~65535 */
static bool parse_port (const char * text, int * port) {
    if (!is_all_digits(text) || strlen(text) > 5) {
        return false;
    }
    long value = strtol(text, NULL, 10);
    if (value < MIN_PORT || value > MAX_PORT) {
        return false;
    }
    *port = (int) value;
    return true;
}

/* (L35) 빈 문자열은 숫자가 아니다. '+'·'-'를 받지 않으려고 strtol 전에 본다 */
static bool is_all_digits (const char * text) {
    if (*text == '\0') {
        return false;
    }
    for (const char * letter = text; *letter != '\0'; letter++) {
        if (*letter < '0' || *letter > '9') {
            return false;
        }
    }
    return true;
}

/* (L15, L35, L36) */
static bool parse_option (const char * text, server_config_t * config) {
    if (strcmp(text, "--no-access-log") == 0) {
        config->access_log = false;
        return true;
    }
    if (strcmp(text, "--count-log") == 0) {
        config->count_log = true;
        return true;
    }
    return parse_timeout_option(text, config);
}

/* (L18, L35) */
static bool parse_timeout_option (const char * text,
                                  server_config_t * config) {
    const timeout_option_t options[] = {
        { "--header-timeout-ms=", &config->header_timeout_ms },
        { "--send-timeout-ms=", &config->send_timeout_ms },
        { "--keepalive-timeout-ms=", &config->keepalive_timeout_ms },
        { "--linger-quiet-ms=", &config->linger_quiet_ms },
        { "--linger-total-ms=", &config->linger_total_ms },
    };

    for (size_t i = 0; i < sizeof(options) / sizeof(options[0]); i++) {
        size_t length = strlen(options[i].prefix);
        if (strncmp(text, options[i].prefix, length) == 0) {
            return parse_milliseconds(text + length, options[i].value);
        }
    }
    return false;
}

/* (L35) 1 이상의 정수. 넘치면(ERANGE) 거절 */
static bool parse_milliseconds (const char * text, long long * value) {
    if (!is_all_digits(text)) {
        return false;
    }
    errno = 0;
    long long parsed = strtoll(text, NULL, 10);
    if (errno == ERANGE || parsed < 1) {
        return false;
    }
    *value = parsed;
    return true;
}

/* (L35) */
static void print_usage (const char * program) {
    fprintf(stderr,
            "usage: %s <port> <static root> [--no-access-log] [--count-log]\n"
            "  [--header-timeout-ms=N] [--send-timeout-ms=N]\n"
            "  [--keepalive-timeout-ms=N] [--linger-quiet-ms=N]\n"
            "  [--linger-total-ms=N]\n", program);
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
