#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
} options_t;

static int parse_options (int argc, char ** argv, options_t * options);
static int read_number (const char * text, long minimum, long maximum,
    long * value);
static void print_usage (const char * program);
static proxy_config_t * load_proxy_config (const options_t * options);

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
    server_options_t server_options;
    server_options.port = options.port;
    server_options.root_path = options.root_path;
    server_options.max_connections = options.max_connections;
    server_options.proxy_config = config;
    server_t * server = create_server(&server_options);
    if (server == NULL) {
        destroy_proxy_config(config);
        return 1;
    }
    write_log(LOG_INFO, "포트 %d에서 시작 (루트 %s)", options.port,
        options.root_path);
    int result = run_server(server);
    destroy_server(server);
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
        "        [--log-level error|info|debug] [--max-connections <수>]\n",
        program);
}

/* 설정 파일이 없으면 프록시 규칙 없이 정적 파일만 서빙한다 */
static proxy_config_t * load_proxy_config (const options_t * options) {
    if (options->config_path == NULL) {
        return create_empty_proxy_config();
    }
    return create_proxy_config(options->config_path);
}
