#include "proxy_config.h"

#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "log.h"
#include "settings.h"

#define HOST_LENGTH_MAX 255
#define TIMEOUT_SECONDS_MAX 3600
#define SEPARATORS " \t\r"

static char * read_file_text (const char * file_path);
static char * copy_text (const char * source, size_t length);
static int parse_text (proxy_config_t * config, const char * text);
static int parse_line (proxy_config_t * config, char * line,
    size_t line_number);
static int parse_route_line (proxy_config_t * config, char ** cursor,
    size_t line_number);
static route_t * add_route (proxy_config_t * config);
static int fill_route_prefix (route_t * route, const char * prefix);
static int add_backend (route_t * route, const char * text);
static int resolve_backend (const char * text, backend_t * backend);
static int parse_timeout_line (int * seconds, char ** cursor,
    size_t line_number);
static int is_prefix_match (const route_t * route, const char * path);

/* ── 공개 함수 ── */

proxy_config_t * create_empty_proxy_config (void) {
    proxy_config_t * config = calloc(1, sizeof(*config));
    if (config == NULL) {
        return NULL;
    }
    config->connect_timeout_seconds = DEFAULT_CONNECT_TIMEOUT_SECONDS;
    config->response_timeout_seconds = DEFAULT_RESPONSE_TIMEOUT_SECONDS;
    config->backend_fail_timeout_seconds =
        DEFAULT_BACKEND_FAIL_TIMEOUT_SECONDS;
    return config;
}

/* 파일을 읽을 수 없거나 형식이 틀리면 NULL. 원인은 로그로 남긴다 */
proxy_config_t * create_proxy_config (const char * file_path) {
    char * text = read_file_text(file_path);
    if (text == NULL) {
        write_log(LOG_ERROR, "설정 파일을 읽을 수 없음: %s", file_path);
        return NULL;
    }
    proxy_config_t * config = create_proxy_config_from_text(text);
    free(text);
    return config;
}

proxy_config_t * create_proxy_config_from_text (const char * text) {
    proxy_config_t * config = create_empty_proxy_config();
    if (config == NULL) {
        return NULL;
    }
    if (parse_text(config, text) != 0) {
        destroy_proxy_config(config);
        return NULL;
    }
    return config;
}

void destroy_proxy_config (proxy_config_t * config) {
    for (size_t i = 0; i < config->route_count; i++) {
        free(config->routes[i].prefix);
        free(config->routes[i].backends);
    }
    free(config->routes);
    free(config);
}

/* 겹치는 규칙이 있으면 가장 긴 경로가 이긴다 (PRX-04) */
route_t * find_route (proxy_config_t * config, const char * path) {
    route_t * best = NULL;
    for (size_t i = 0; i < config->route_count; i++) {
        route_t * route = &config->routes[i];
        if (!is_prefix_match(route, path)) {
            continue;
        }
        if (best == NULL || route->prefix_length > best->prefix_length) {
            best = route;
        }
    }
    return best;
}

/* ── 도우미 함수 ── */

/* 새 메모리를 돌려준다. 받은 쪽이 free한다. 읽지 못하면 NULL */
static char * read_file_text (const char * file_path) {
    FILE * file = fopen(file_path, "r");
    if (file == NULL) {
        return NULL;
    }
    char * text = copy_text("", 0);
    size_t length = 0;
    char chunk[4096];
    size_t received = 0;
    while (text != NULL && (received = fread(chunk, 1, sizeof(chunk),
            file)) > 0) {
        char * grown = realloc(text, length + received + 1);
        if (grown == NULL) {
            free(text);
            text = NULL;
            break;
        }
        text = grown;
        memcpy(text + length, chunk, received);
        length += received;
        text[length] = '\0';
    }
    fclose(file);
    return text;
}

/* 새 메모리를 돌려준다. 받은 쪽이 free한다 */
static char * copy_text (const char * source, size_t length) {
    char * copy = malloc(length + 1);
    if (copy == NULL) {
        return NULL;
    }
    memcpy(copy, source, length);
    copy[length] = '\0';
    return copy;
}

static int parse_text (proxy_config_t * config, const char * text) {
    size_t line_number = 0;
    while (*text != '\0') {
        const char * line_end = strchr(text, '\n');
        size_t length = line_end == NULL ? strlen(text)
            : (size_t) (line_end - text);
        char * line = copy_text(text, length);
        if (line == NULL) {
            return -1;
        }
        line_number++;
        int result = parse_line(config, line, line_number);
        free(line);
        if (result != 0) {
            return -1;
        }
        text += line_end == NULL ? length : length + 1;
    }
    return 0;
}

/* line은 호출자 소유다. strtok_r이 내용을 바꾸므로 읽는 용도로 쓰면 안 된다 */
static int parse_line (proxy_config_t * config, char * line,
    size_t line_number) {
    char * cursor = NULL;
    char * command = strtok_r(line, SEPARATORS, &cursor);
    if (command == NULL) {
        return 0;
    }
    if (strcmp(command, "route") == 0) {
        return parse_route_line(config, &cursor, line_number);
    }
    if (strcmp(command, "connect_timeout") == 0) {
        return parse_timeout_line(&config->connect_timeout_seconds, &cursor,
            line_number);
    }
    if (strcmp(command, "response_timeout") == 0) {
        return parse_timeout_line(&config->response_timeout_seconds, &cursor,
            line_number);
    }
    if (strcmp(command, "backend_fail_timeout") == 0) {
        return parse_timeout_line(&config->backend_fail_timeout_seconds,
            &cursor, line_number);
    }
    write_log(LOG_ERROR, "설정 %zu행: 알 수 없는 명령 %s", line_number,
        command);
    return -1;
}

static int parse_route_line (proxy_config_t * config, char ** cursor,
    size_t line_number) {
    char * prefix = strtok_r(NULL, SEPARATORS, cursor);
    route_t * route = prefix == NULL ? NULL : add_route(config);
    if (route == NULL || fill_route_prefix(route, prefix) != 0) {
        write_log(LOG_ERROR, "설정 %zu행: route 형식 오류", line_number);
        return -1;
    }
    char * address = strtok_r(NULL, SEPARATORS, cursor);
    while (address != NULL) {
        if (add_backend(route, address) != 0) {
            write_log(LOG_ERROR, "설정 %zu행: 백엔드 주소 오류 %s",
                line_number, address);
            return -1;
        }
        address = strtok_r(NULL, SEPARATORS, cursor);
    }
    if (route->backend_count == 0) {
        write_log(LOG_ERROR, "설정 %zu행: 백엔드가 없음", line_number);
        return -1;
    }
    return 0;
}

/* 비어 있는 새 규칙의 자리를 돌려준다. 실패하면 NULL */
static route_t * add_route (proxy_config_t * config) {
    route_t * grown = realloc(config->routes,
        (config->route_count + 1) * sizeof(*grown));
    if (grown == NULL) {
        return NULL;
    }
    config->routes = grown;
    route_t * route = &config->routes[config->route_count];
    memset(route, 0, sizeof(*route));
    config->route_count++;
    return route;
}

/* 끝의 /는 떼어 둬야 경로 조각 단위로 비교할 수 있다 (PRX-03) */
static int fill_route_prefix (route_t * route, const char * prefix) {
    size_t length = strlen(prefix);
    if (length == 0 || prefix[0] != '/') {
        return -1;
    }
    while (length > 1 && prefix[length - 1] == '/') {
        length--;
    }
    route->prefix = copy_text(prefix, length);
    route->prefix_length = length;
    return route->prefix == NULL ? -1 : 0;
}

static int add_backend (route_t * route, const char * text) {
    backend_t * grown = realloc(route->backends,
        (route->backend_count + 1) * sizeof(*grown));
    if (grown == NULL) {
        return -1;
    }
    route->backends = grown;
    if (resolve_backend(text, &route->backends[route->backend_count]) != 0) {
        return -1;
    }
    route->backend_count++;
    return 0;
}

/* 주소 해석은 시작할 때 한 번만 한다. 요청 처리 중에는 부르지 않는다 (PRX-02) */
static int resolve_backend (const char * text, backend_t * backend) {
    const char * colon = strrchr(text, ':');
    if (colon == NULL || colon[1] == '\0') {
        return -1;
    }
    const char * host_start = text;
    size_t host_length = (size_t) (colon - text);
    if (host_length >= 2 && text[0] == '[' && colon[-1] == ']') {
        host_start++;
        host_length -= 2;
    }
    if (host_length == 0 || host_length > HOST_LENGTH_MAX) {
        return -1;
    }
    char * host = copy_text(host_start, host_length);
    if (host == NULL) {
        return -1;
    }
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_NUMERICSERV;
    struct addrinfo * found = NULL;
    int result = getaddrinfo(host, colon + 1, &hints, &found);
    free(host);
    if (result != 0) {
        return -1;
    }
    memcpy(&backend->address, found->ai_addr, found->ai_addrlen);
    backend->address_length = found->ai_addrlen;
    freeaddrinfo(found);
    return 0;
}

static int parse_timeout_line (int * seconds, char ** cursor,
    size_t line_number) {
    char * value = strtok_r(NULL, SEPARATORS, cursor);
    char * end = NULL;
    long parsed = value == NULL ? 0 : strtol(value, &end, 10);
    if (value == NULL || *end != '\0' || parsed <= 0
        || parsed > TIMEOUT_SECONDS_MAX
        || strtok_r(NULL, SEPARATORS, cursor) != NULL) {
        write_log(LOG_ERROR, "설정 %zu행: 타임아웃 값 오류", line_number);
        return -1;
    }
    *seconds = (int) parsed;
    return 0;
}

/* /api 규칙은 /api, /api/users와 맞고 /apix와는 맞지 않는다 */
static int is_prefix_match (const route_t * route, const char * path) {
    if (route->prefix_length == 1) {
        return 1;
    }
    if (strncmp(path, route->prefix, route->prefix_length) != 0) {
        return 0;
    }
    char next = path[route->prefix_length];
    return next == '\0' || next == '/';
}
