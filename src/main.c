#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "log.h"
#include "proxy_config.h"
#include "server.h"
#include "settings.h"
#include "signals.h"

typedef struct {
    int port;
    const char * root_path;
    const char * config_path;
    log_level_t log_level;
    size_t max_connections;
    size_t loop_count;
} options_t;

/* server_t 하나 = 이벤트 루프 하나 = 스레드 하나 (M1, M7) */
typedef struct {
    server_t * server;
    pthread_t thread;
    int result;
} loop_t;

static int parse_options (int argc, char ** argv, options_t * options);
static size_t count_default_loops (void);
static int read_number (const char * text, long minimum, long maximum,
    long * value);
static void print_usage (const char * program);
static proxy_config_t * load_proxy_config (const options_t * options);
static loop_t * create_loops (const options_t * options,
    proxy_config_t * config, upstream_t * upstream);
static void destroy_loops (loop_t * loops, size_t count);
static int run_loops (loop_t * loops, size_t count);
static size_t start_loop_threads (loop_t * loops, size_t count);
static void * run_loop_thread (void * argument);

/* ── 공개 함수 ── */

int main (int argc, char ** argv) {
    options_t options;
    if (parse_options(argc, argv, &options) != 0) {
        print_usage(argv[0]);
        return 2;
    }
    set_log_level(options.log_level);
    if (install_signal_handlers() != 0) {
        write_log(LOG_ERROR, "시그널 설정 실패: %s", strerror(errno));
        return 1;
    }
    proxy_config_t * config = load_proxy_config(&options);
    if (config == NULL) {
        return 1;
    }
    raise_fd_limit(options.max_connections, options.loop_count);
    options.max_connections = compute_connection_limit(options.max_connections,
        options.loop_count);
    if (options.max_connections == 0) {
        write_log(LOG_ERROR, "fd 한도가 너무 낮아 손님을 받을 수 없음");
        destroy_proxy_config(config);
        return 1;
    }
    upstream_t * upstream = create_upstream(config);
    loop_t * loops = upstream == NULL ? NULL
        : create_loops(&options, config, upstream);
    if (loops == NULL) {
        destroy_upstream(upstream);
        destroy_proxy_config(config);
        return 1;
    }
    write_log(LOG_INFO, "포트 %d에서 시작 (루트 %s, 루프 %zu개)", options.port,
        options.root_path, options.loop_count);
    int result = run_loops(loops, options.loop_count);
    destroy_loops(loops, options.loop_count);
    destroy_upstream(upstream);
    destroy_proxy_config(config);
    write_log(LOG_INFO, "정상 종료");
    return result == 0 ? 0 : 1;
}

/* ── 도우미 함수 ── */

static int parse_options (int argc, char ** argv, options_t * options) {
    options->port = DEFAULT_PORT;
    options->root_path = NULL;
    options->config_path = NULL;
    options->log_level = LOG_INFO;
    options->max_connections = DEFAULT_MAX_CONNECTIONS;
    options->loop_count = count_default_loops();
    for (int i = 1; i < argc; i += 2) {
        const char * name = argv[i];
        const char * value = i + 1 < argc ? argv[i + 1] : NULL;
        long number = 0;
        if (value == NULL) {
            return -1;
        }
        if (strcmp(name, "--port") == 0 && read_number(value, 1, 65535,
                &number) == 0) {
            options->port = (int) number;
        } else if (strcmp(name, "--max-connections") == 0
            && read_number(value, 1, 1000000, &number) == 0) {
            options->max_connections = (size_t) number;
        } else if (strcmp(name, "--loops") == 0
            && read_number(value, 1, LOOP_COUNT_MAX, &number) == 0) {
            options->loop_count = (size_t) number;
        } else if (strcmp(name, "--root") == 0) {
            options->root_path = value;
        } else if (strcmp(name, "--config") == 0) {
            options->config_path = value;
        } else if (strcmp(name, "--log-level") == 0
            && parse_log_level(value, &options->log_level) == 0) {
            continue;
        } else {
            return -1;
        }
    }
    return options->root_path == NULL ? -1 : 0;
}

/*
 * 코어마다 루프 하나 (M2). macOS는 SO_REUSEPORT가 연결을 고르게 나눠 준다는
 * 보장이 없어 하나만 쓴다. sysconf는 docker --cpus 제한을 모르므로 그때는
 * --loops로 맞춘다.
 */
static size_t count_default_loops (void) {
#ifdef __APPLE__
    return 1;
#else
    long online = sysconf(_SC_NPROCESSORS_ONLN);
    if (online < 1) {
        return 1;
    }
    return online > LOOP_COUNT_MAX ? LOOP_COUNT_MAX : (size_t) online;
#endif
}

static int read_number (const char * text, long minimum, long maximum,
    long * value) {
    char * end = NULL;
    errno = 0;
    long parsed = strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || parsed < minimum
        || parsed > maximum) {
        return -1;
    }
    *value = parsed;
    return 0;
}

static void print_usage (const char * program) {
    fprintf(stderr,
        "사용법: %s --root <폴더> [--port <번호>] [--config <파일>]\n"
        "        [--log-level error|info|debug] [--max-connections <수>]\n"
        "        [--loops <1~256>]\n",
        program);
}

/* 설정 파일이 없으면 프록시 규칙 없이 정적 파일만 서빙한다 */
static proxy_config_t * load_proxy_config (const options_t * options) {
    if (options->config_path == NULL) {
        return create_empty_proxy_config();
    }
    return create_proxy_config(options->config_path);
}

/*
 * 스레드를 만들기 전에 루프를 모두 만든다. 포트를 못 여는 등 하나라도
 * 실패하면 만든 것을 정리하고 NULL (M7)
 */
static loop_t * create_loops (const options_t * options,
    proxy_config_t * config, upstream_t * upstream) {
    loop_t * loops = calloc(options->loop_count, sizeof(*loops));
    if (loops == NULL) {
        return NULL;
    }
    server_options_t server_options;
    server_options.port = options->port;
    server_options.root_path = options->root_path;
    server_options.max_connections = options->max_connections;
    server_options.proxy_config = config;
    server_options.upstream = upstream;
    for (size_t i = 0; i < options->loop_count; i++) {
        loops[i].server = create_server(&server_options);
        if (loops[i].server == NULL) {
            destroy_loops(loops, i);
            return NULL;
        }
    }
    return loops;
}

static void destroy_loops (loop_t * loops, size_t count) {
    for (size_t i = 0; i < count; i++) {
        destroy_server(loops[i].server);
    }
    free(loops);
}

/*
 * 모든 루프 스레드가 끝날 때까지 기다린다 (M6). 0이면 모든 루프가 정상
 * 종료했다. 스레드를 다 만들지 못하면 만든 루프도 종료 절차를 밟게 한다.
 */
static int run_loops (loop_t * loops, size_t count) {
    size_t started = start_loop_threads(loops, count);
    int result = 0;
    if (started < count) {
        write_log(LOG_ERROR, "루프 스레드를 만들 수 없음 (%zu / %zu)", started,
            count);
        request_shutdown();
        result = -1;
    }
    for (size_t i = 0; i < started; i++) {
        pthread_join(loops[i].thread, NULL);
        if (loops[i].result != 0) {
            result = -1;
        }
    }
    return result;
}

/* 만든 스레드 수를 돌려준다. 앞에서부터 차례로 만든다 */
static size_t start_loop_threads (loop_t * loops, size_t count) {
    for (size_t i = 0; i < count; i++) {
        if (pthread_create(&loops[i].thread, NULL, run_loop_thread,
                &loops[i]) != 0) {
            return i;
        }
    }
    return count;
}

/* 루프 하나가 오류로 멈추면 서버 전체를 종료한다. 루프 하나일 때와 같다 (M6) */
static void * run_loop_thread (void * argument) {
    loop_t * loop = argument;
    loop->result = run_server(loop->server);
    if (loop->result != 0) {
        request_shutdown();
    }
    return NULL;
}
