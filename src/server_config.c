#include "server_config.h"

#include <arpa/inet.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#include "http_request.h"
#include "static_file.h"

/* (L5, L11, L12, G11, G13) 4번 표 기본값. 손님 쪽은 nginx 기본값 */
#define DEFAULT_HEADER_TIMEOUT_MS 60000
#define DEFAULT_BODY_TIMEOUT_MS 60000
#define DEFAULT_SEND_TIMEOUT_MS 60000
#define DEFAULT_KEEPALIVE_TIMEOUT_MS 75000
#define DEFAULT_LINGER_QUIET_MS 5000
#define DEFAULT_LINGER_TOTAL_MS 30000
#define DEFAULT_BACKEND_CONNECT_TIMEOUT_MS 5000
#define DEFAULT_BACKEND_SEND_TIMEOUT_MS 60000
#define DEFAULT_BACKEND_READ_TIMEOUT_MS 60000
#define DEFAULT_BACKEND_FAIL_TIMEOUT_MS 10000
/* (B4, G9) */
#define DEFAULT_MAX_BODY MAX_BODY_SIZE
#define DEFAULT_MAX_BACKEND_RESPONSE (8 * 1024 * 1024)

/* (G15) */
#define MIN_PORT 1
#define MAX_PORT 65535
#define MAX_PORT_DIGITS 5
/* (G40) K = 1024. B4의 1MB(1,048,576)와 맞춘다 */
#define KILOBYTE 1024ULL
#define MEGABYTE (1024ULL * 1024ULL)
/* (G15) epoll_wait 대기 시간이 int ms라 그보다 긴 타임아웃은 받지 않는다 */
#define MAX_TIME_MS ((unsigned long long) INT_MAX)
/* (G27) 응답 버퍼 = 헤더 8KB + 본문이라 더해도 size_t가 넘치지 않게 */
#define MAX_SIZE_VALUE ((unsigned long long) (SIZE_MAX - MAX_HEADER_SIZE))

/* (G15) 한 줄에 "키 값...", 공백·탭으로 나눈다. CRLF 파일도 받는다 */
static const char WORD_DELIMITERS[] = " \t\r\n";

/* (G15) 4번 표 항목의 값 종류 */
typedef enum {
    VALUE_PORT,
    VALUE_PATH,
    VALUE_TIME,
    VALUE_SIZE,
    VALUE_SWITCH
} value_kind_t;

/* (G15) route를 뺀 항목 하나. target은 읽은 값을 쓸 자리 (빌림) */
typedef struct {
    const char * key;
    value_kind_t kind;
    void * target;
    /* 처음 나온 줄. 0 = 아직 안 나옴 (route 외 중복은 오류) */
    size_t line;
} setting_t;

/* (G15, G31) 읽는 동안의 상태. 오류는 error에 쓰고 result에 종류를 남긴다 */
typedef struct {
    const char * path;
    size_t line_number;
    char * error;
    size_t error_size;
    config_result_t result;
    server_config_t * config;
    setting_t * settings;
    size_t setting_count;
} config_parser_t;

static void set_default_config (server_config_t * config);
static config_result_t read_settings (const char * path,
                                      server_config_t * config, char * error,
                                      size_t error_size);
static bool read_config_file (config_parser_t * parser);
static void report_config_error (config_parser_t * parser,
                                 config_result_t result,
                                 const char * format, ...)
    __attribute__((format(printf, 3, 4)));
static bool read_config_lines (config_parser_t * parser, FILE * file);
static bool parse_config_line (config_parser_t * parser, char * line);
static void cut_comment (char * line);
static bool parse_route (config_parser_t * parser, char ** save);
static bool fill_route_head (config_parser_t * parser, const char * prefix,
                             const char * balance, route_t * route);
static size_t find_prefix_length (const char * prefix);
static bool has_route_prefix (const server_config_t * config,
                              const char * prefix, size_t length);
static bool parse_balance (const char * text, balance_t * balance);
static char * copy_text (const char * source, size_t length);
static bool add_route_backends (config_parser_t * parser, char ** save,
                                route_t * route);
static bool add_route_backend (config_parser_t * parser, const char * text,
                               route_t * route);
static bool parse_backend_address (const char * text,
                                   struct sockaddr_in * address);
static bool parse_port (const char * text, int * port);
static bool is_all_digits (const char * text, size_t length);
static bool find_or_add_backend (config_parser_t * parser,
                                 const struct sockaddr_in * address,
                                 size_t * index);
static bool has_backend_index (const route_t * route, size_t index);
static bool append_backend_index (route_t * route, size_t index);
static void destroy_route (route_t * route);
static bool append_route (config_parser_t * parser, const route_t * route);
static setting_t * find_setting (config_parser_t * parser, const char * key);
static bool parse_setting_value (config_parser_t * parser,
                                 const setting_t * setting,
                                 const char * value);
static bool parse_time_value (config_parser_t * parser, const char * key,
                              const char * text, long long * target);
static bool read_number (const char * text, size_t length,
                         unsigned long long limit,
                         unsigned long long * number);
static bool parse_size_value (config_parser_t * parser, const char * key,
                              const char * text, size_t * target);
static bool parse_switch_value (config_parser_t * parser, const char * key,
                                const char * text, bool * target);
static bool copy_path_value (config_parser_t * parser, const char * text,
                             char ** target);
static bool check_required (config_parser_t * parser);
static size_t pick_later_line (size_t first, size_t second);
static bool open_root (config_parser_t * parser, const char * root_text);

/* ── 공개 함수 ── */

/* (G15, G31) 다 읽고 검사가 끝난 뒤에만 config에 옮긴다 */
config_result_t load_server_config (const char * path,
                                    server_config_t * config, char * error,
                                    size_t error_size) {
    server_config_t loaded;

    set_default_config(&loaded);
    error[0] = '\0';
    config_result_t result = read_settings(path, &loaded, error, error_size);
    if (result != CONFIG_OK) {
        destroy_server_config(&loaded);
        return result;
    }
    *config = loaded;
    return CONFIG_OK;
}

/* (G15) */
void destroy_server_config (server_config_t * config) {
    for (size_t i = 0; i < config->route_count; i++) {
        destroy_route(&config->routes[i]);
    }
    free(config->routes);
    free(config->backends);
    free(config->root);
    memset(config, 0, sizeof(*config));
}

/* ── 도우미 함수 ── */

/* (G15) 4번 표 기본값. 포인터는 NULL이라 실패해도 destroy가 안전하다 */
static void set_default_config (server_config_t * config) {
    memset(config, 0, sizeof(*config));
    config->access_log = true;
    config->count_log = false;
    config->header_timeout_ms = DEFAULT_HEADER_TIMEOUT_MS;
    config->body_timeout_ms = DEFAULT_BODY_TIMEOUT_MS;
    config->send_timeout_ms = DEFAULT_SEND_TIMEOUT_MS;
    config->keepalive_timeout_ms = DEFAULT_KEEPALIVE_TIMEOUT_MS;
    config->linger_quiet_ms = DEFAULT_LINGER_QUIET_MS;
    config->linger_total_ms = DEFAULT_LINGER_TOTAL_MS;
    config->backend_connect_timeout_ms = DEFAULT_BACKEND_CONNECT_TIMEOUT_MS;
    config->backend_send_timeout_ms = DEFAULT_BACKEND_SEND_TIMEOUT_MS;
    config->backend_read_timeout_ms = DEFAULT_BACKEND_READ_TIMEOUT_MS;
    config->backend_fail_timeout_ms = DEFAULT_BACKEND_FAIL_TIMEOUT_MS;
    config->max_body = DEFAULT_MAX_BODY;
    config->max_backend_response = DEFAULT_MAX_BACKEND_RESPONSE;
}

/* (G15, L35) 4번 표 항목. 파일 읽기 → 필수 항목·값 사이 검사 → root 확인 */
static config_result_t read_settings (const char * path,
                                      server_config_t * config, char * error,
                                      size_t error_size) {
    char * root_text = NULL;
    setting_t settings[] = {
        { "listen", VALUE_PORT, &config->port, 0 },
        { "root", VALUE_PATH, &root_text, 0 },
        { "header_timeout", VALUE_TIME, &config->header_timeout_ms, 0 },
        { "body_timeout", VALUE_TIME, &config->body_timeout_ms, 0 },
        { "send_timeout", VALUE_TIME, &config->send_timeout_ms, 0 },
        { "keepalive_timeout", VALUE_TIME, &config->keepalive_timeout_ms, 0 },
        { "linger_quiet", VALUE_TIME, &config->linger_quiet_ms, 0 },
        { "linger_total", VALUE_TIME, &config->linger_total_ms, 0 },
        { "backend_connect_timeout", VALUE_TIME,
          &config->backend_connect_timeout_ms, 0 },
        { "backend_send_timeout", VALUE_TIME,
          &config->backend_send_timeout_ms, 0 },
        { "backend_read_timeout", VALUE_TIME,
          &config->backend_read_timeout_ms, 0 },
        { "backend_fail_timeout", VALUE_TIME,
          &config->backend_fail_timeout_ms, 0 },
        { "max_body", VALUE_SIZE, &config->max_body, 0 },
        { "max_backend_response", VALUE_SIZE,
          &config->max_backend_response, 0 },
        { "access_log", VALUE_SWITCH, &config->access_log, 0 },
        { "count_log", VALUE_SWITCH, &config->count_log, 0 },
    };
    config_parser_t parser = {
        .path = path,
        .line_number = 0,
        .error = error,
        .error_size = error_size,
        .result = CONFIG_OK,
        .config = config,
        .settings = settings,
        .setting_count = sizeof(settings) / sizeof(settings[0]),
    };

    bool is_loaded = read_config_file(&parser) && check_required(&parser)
                     && open_root(&parser, root_text);
    free(root_text);
    return is_loaded ? CONFIG_OK : parser.result;
}

/* (G31) 파일을 못 열면 환경 문제 */
static bool read_config_file (config_parser_t * parser) {
    FILE * file = fopen(parser->path, "r");

    if (file == NULL) {
        report_config_error(parser, CONFIG_UNAVAILABLE, "%s",
                            strerror(errno));
        return false;
    }
    bool is_read = read_config_lines(parser, file);
    fclose(file);
    return is_read;
}

/* (G15, G31) 줄 번호가 0이면 "파일: 이유", 아니면 "파일:줄: 이유" */
static void report_config_error (config_parser_t * parser,
                                 config_result_t result,
                                 const char * format, ...) {
    va_list arguments;
    int written = 0;

    parser->result = result;
    if (parser->line_number == 0) {
        written = snprintf(parser->error, parser->error_size, "%s: ",
                           parser->path);
    } else {
        written = snprintf(parser->error, parser->error_size, "%s:%zu: ",
                           parser->path, parser->line_number);
    }
    if (written < 0 || (size_t) written >= parser->error_size) {
        return;
    }
    va_start(arguments, format);
    vsnprintf(parser->error + written, parser->error_size - (size_t) written,
              format, arguments);
    va_end(arguments);
}

/* (G15) getline이라 줄 길이 상한이 없다 */
static bool read_config_lines (config_parser_t * parser, FILE * file) {
    char * line = NULL;
    size_t capacity = 0;
    bool is_valid = true;

    while (is_valid && getline(&line, &capacity, file) >= 0) {
        parser->line_number++;
        is_valid = parse_config_line(parser, line);
    }
    if (is_valid && ferror(file)) {
        report_config_error(parser, CONFIG_UNAVAILABLE, "read failed");
        is_valid = false;
    }
    free(line);
    return is_valid;
}

/* (G15) 빈 줄·주석은 건너뛰고, route 외 항목은 값이 정확히 하나 */
static bool parse_config_line (config_parser_t * parser, char * line) {
    char * save = NULL;

    cut_comment(line);
    char * key = strtok_r(line, WORD_DELIMITERS, &save);
    if (key == NULL) {
        return true;
    }
    if (strcmp(key, "route") == 0) {
        return parse_route(parser, &save);
    }
    setting_t * setting = find_setting(parser, key);
    if (setting == NULL) {
        report_config_error(parser, CONFIG_INVALID, "unknown key %s", key);
        return false;
    }
    if (setting->line != 0) {
        report_config_error(parser, CONFIG_INVALID,
                            "duplicate %s (first on line %zu)", key,
                            setting->line);
        return false;
    }
    char * value = strtok_r(NULL, WORD_DELIMITERS, &save);
    if (value == NULL) {
        report_config_error(parser, CONFIG_INVALID, "missing value for %s",
                            key);
        return false;
    }
    if (strtok_r(NULL, WORD_DELIMITERS, &save) != NULL) {
        report_config_error(parser, CONFIG_INVALID,
                            "too many values for %s", key);
        return false;
    }
    if (!parse_setting_value(parser, setting, value)) {
        return false;
    }
    setting->line = parser->line_number;
    return true;
}

/* (G15, G46) 줄 어디에 있든 '#' 뒤는 주석 */
static void cut_comment (char * line) {
    char * mark = strchr(line, '#');

    if (mark != NULL) {
        *mark = '\0';
    }
}

/* (G1, G2, G14, G15) route <접두사> <round_robin|least_conn> <IP:포트>... */
static bool parse_route (config_parser_t * parser, char ** save) {
    route_t route = {0};
    const char * prefix = strtok_r(NULL, WORD_DELIMITERS, save);
    const char * balance = strtok_r(NULL, WORD_DELIMITERS, save);

    if (prefix == NULL || balance == NULL) {
        report_config_error(parser, CONFIG_INVALID,
                            "route needs <prefix> <round_robin|least_conn> "
                            "<IP:port>...");
        return false;
    }
    if (!fill_route_head(parser, prefix, balance, &route)) {
        return false;
    }
    if (!add_route_backends(parser, save, &route)) {
        destroy_route(&route);
        return false;
    }
    if (!append_route(parser, &route)) {
        destroy_route(&route);
        return false;
    }
    return true;
}

/* (G2, G15) '/'로 시작, 끝 '/' 정리, 같은 접두사 두 번은 오류.
 * 검사를 다 끝낸 뒤에 접두사를 복사한다 */
static bool fill_route_head (config_parser_t * parser, const char * prefix,
                             const char * balance, route_t * route) {
    if (prefix[0] != '/') {
        report_config_error(parser, CONFIG_INVALID,
                            "route prefix must start with /: %s", prefix);
        return false;
    }
    size_t length = find_prefix_length(prefix);
    if (has_route_prefix(parser->config, prefix, length)) {
        report_config_error(parser, CONFIG_INVALID,
                            "duplicate route prefix %.*s", (int) length,
                            prefix);
        return false;
    }
    if (!parse_balance(balance, &route->balance)) {
        report_config_error(parser, CONFIG_INVALID,
                            "balance must be round_robin or least_conn: %s",
                            balance);
        return false;
    }
    route->prefix = copy_text(prefix, length);
    if (route->prefix == NULL) {
        report_config_error(parser, CONFIG_UNAVAILABLE, "out of memory");
        return false;
    }
    route->prefix_length = length;
    return true;
}

/* (G15, G40) 끝 '/'를 뺀 길이. "/"만 있으면 그대로 1 (전부 백엔드) */
static size_t find_prefix_length (const char * prefix) {
    size_t length = strlen(prefix);

    while (length > 1 && prefix[length - 1] == '/') {
        length--;
    }
    return length;
}

/* (G15) */
static bool has_route_prefix (const server_config_t * config,
                              const char * prefix, size_t length) {
    for (size_t i = 0; i < config->route_count; i++) {
        const route_t * route = &config->routes[i];
        if (route->prefix_length == length
            && memcmp(route->prefix, prefix, length) == 0) {
            return true;
        }
    }
    return false;
}

/* (G14) */
static bool parse_balance (const char * text, balance_t * balance) {
    if (strcmp(text, "round_robin") == 0) {
        *balance = BALANCE_ROUND_ROBIN;
        return true;
    }
    if (strcmp(text, "least_conn") == 0) {
        *balance = BALANCE_LEAST_CONN;
        return true;
    }
    return false;
}

/* (G15) 반환: '\0'으로 끝나는 새 문자열, NULL = 할당 실패. 받은 쪽이 free */
static char * copy_text (const char * source, size_t length) {
    char * copy = malloc(length + 1);
    if (copy == NULL) {
        return NULL;
    }
    memcpy(copy, source, length);
    copy[length] = '\0';
    return copy;
}

/* (G14, G15) 백엔드는 하나 이상 */
static bool add_route_backends (config_parser_t * parser, char ** save,
                                route_t * route) {
    const char * text = strtok_r(NULL, WORD_DELIMITERS, save);

    while (text != NULL) {
        if (!add_route_backend(parser, text, route)) {
            return false;
        }
        text = strtok_r(NULL, WORD_DELIMITERS, save);
    }
    if (route->backend_count == 0) {
        report_config_error(parser, CONFIG_INVALID,
                            "route needs at least one IP:port");
        return false;
    }
    return true;
}

/* (G15, G29, G40) IP:포트만. 같은 주소는 백엔드 하나, 한 규칙에 두 번은 오류 */
static bool add_route_backend (config_parser_t * parser, const char * text,
                               route_t * route) {
    struct sockaddr_in address;
    size_t index = 0;

    if (!parse_backend_address(text, &address)) {
        report_config_error(parser, CONFIG_INVALID,
                            "backend must be IP:port: %s", text);
        return false;
    }
    if (!find_or_add_backend(parser, &address, &index)) {
        return false;
    }
    if (has_backend_index(route, index)) {
        report_config_error(parser, CONFIG_INVALID,
                            "duplicate backend %s in route", text);
        return false;
    }
    if (!append_backend_index(route, index)) {
        report_config_error(parser, CONFIG_UNAVAILABLE, "out of memory");
        return false;
    }
    return true;
}

/* (G15, D1) 호스트 이름은 받지 않는다 (요청마다 DNS 조회를 피함) */
static bool parse_backend_address (const char * text,
                                   struct sockaddr_in * address) {
    char ip[INET_ADDRSTRLEN];
    const char * colon = strrchr(text, ':');
    int port = 0;

    if (colon == NULL || (size_t) (colon - text) >= sizeof(ip)) {
        return false;
    }
    memcpy(ip, text, (size_t) (colon - text));
    ip[colon - text] = '\0';
    memset(address, 0, sizeof(*address));
    address->sin_family = AF_INET;
    if (inet_pton(AF_INET, ip, &address->sin_addr) != 1) {
        return false;
    }
    if (!parse_port(colon + 1, &port)) {
        return false;
    }
    address->sin_port = htons((uint16_t) port);
    return true;
}

/* (G15, L35) 숫자만, 1~65535. '+'·'-'는 받지 않는다 */
static bool parse_port (const char * text, int * port) {
    size_t length = strlen(text);

    if (!is_all_digits(text, length) || length > MAX_PORT_DIGITS) {
        return false;
    }
    long value = strtol(text, NULL, 10);
    if (value < MIN_PORT || value > MAX_PORT) {
        return false;
    }
    *port = (int) value;
    return true;
}

/* (G15) 빈 글자는 숫자가 아니다 */
static bool is_all_digits (const char * text, size_t length) {
    if (length == 0) {
        return false;
    }
    for (size_t i = 0; i < length; i++) {
        if (text[i] < '0' || text[i] > '9') {
            return false;
        }
    }
    return true;
}

/* (G29) *index: config->backends 안의 번호 */
static bool find_or_add_backend (config_parser_t * parser,
                                 const struct sockaddr_in * address,
                                 size_t * index) {
    server_config_t * config = parser->config;

    for (size_t i = 0; i < config->backend_count; i++) {
        if (config->backends[i].sin_addr.s_addr == address->sin_addr.s_addr
            && config->backends[i].sin_port == address->sin_port) {
            *index = i;
            return true;
        }
    }
    struct sockaddr_in * grown = realloc(config->backends,
                                         (config->backend_count + 1)
                                         * sizeof(*grown));
    if (grown == NULL) {
        report_config_error(parser, CONFIG_UNAVAILABLE, "out of memory");
        return false;
    }
    grown[config->backend_count] = *address;
    config->backends = grown;
    *index = config->backend_count;
    config->backend_count++;
    return true;
}

/* (G40) */
static bool has_backend_index (const route_t * route, size_t index) {
    for (size_t i = 0; i < route->backend_count; i++) {
        if (route->backend_indexes[i] == index) {
            return true;
        }
    }
    return false;
}

/* (G14) 적힌 순서 그대로 붙인다 (라운드로빈 순서) */
static bool append_backend_index (route_t * route, size_t index) {
    size_t * grown = realloc(route->backend_indexes,
                             (route->backend_count + 1) * sizeof(*grown));
    if (grown == NULL) {
        return false;
    }
    grown[route->backend_count] = index;
    route->backend_indexes = grown;
    route->backend_count++;
    return true;
}

/* (G15) 두 번 불러도 안전하다 */
static void destroy_route (route_t * route) {
    free(route->prefix);
    free(route->backend_indexes);
    memset(route, 0, sizeof(*route));
}

/* (G2) 적힌 순서 그대로. route: 성공하면 넘겨받는다. 실패하면 호출자가 정리 */
static bool append_route (config_parser_t * parser, const route_t * route) {
    server_config_t * config = parser->config;
    route_t * grown = realloc(config->routes,
                              (config->route_count + 1) * sizeof(*grown));

    if (grown == NULL) {
        report_config_error(parser, CONFIG_UNAVAILABLE, "out of memory");
        return false;
    }
    grown[config->route_count] = *route;
    config->routes = grown;
    config->route_count++;
    return true;
}

/* (G15) 반환: NULL = 모르는 키 */
static setting_t * find_setting (config_parser_t * parser, const char * key) {
    for (size_t i = 0; i < parser->setting_count; i++) {
        if (strcmp(parser->settings[i].key, key) == 0) {
            return &parser->settings[i];
        }
    }
    return NULL;
}

/* (G15) 오류는 각 도우미가 알린다 */
static bool parse_setting_value (config_parser_t * parser,
                                 const setting_t * setting,
                                 const char * value) {
    switch (setting->kind) {
    case VALUE_PORT:
        if (!parse_port(value, setting->target)) {
            report_config_error(parser, CONFIG_INVALID,
                                "%s must be a port 1-65535: %s",
                                setting->key, value);
            return false;
        }
        return true;
    case VALUE_PATH:
        return copy_path_value(parser, value, setting->target);
    case VALUE_TIME:
        return parse_time_value(parser, setting->key, value, setting->target);
    case VALUE_SIZE:
        return parse_size_value(parser, setting->key, value, setting->target);
    case VALUE_SWITCH:
        return parse_switch_value(parser, setting->key, value,
                                  setting->target);
    }
    return false;
}

/* (G15) 숫자 + ms 또는 s (단위 필수), 0보다 크고 int ms 안 */
static bool parse_time_value (config_parser_t * parser, const char * key,
                              const char * text, long long * target) {
    size_t digits = strspn(text, "0123456789");
    const char * unit = text + digits;
    unsigned long long scale = 0;
    unsigned long long number = 0;

    if (strcmp(unit, "ms") == 0) {
        scale = 1;
    } else if (strcmp(unit, "s") == 0) {
        scale = 1000;
    }
    if (scale == 0 || !read_number(text, digits, MAX_TIME_MS / scale, &number)
        || number == 0) {
        report_config_error(parser, CONFIG_INVALID,
                            "%s must be a time like 500ms or 60s "
                            "(1ms-%llums): %s",
                            key, MAX_TIME_MS, text);
        return false;
    }
    *target = (long long) (number * scale);
    return true;
}

/* (G15) 상한을 넘는 순간 멈춰서 넘침으로 작은 값이 되지 않게 한다.
 * 반환: false = 숫자가 없거나 limit 초과 */
static bool read_number (const char * text, size_t length,
                         unsigned long long limit,
                         unsigned long long * number) {
    unsigned long long total = 0;

    if (length == 0) {
        return false;
    }
    for (size_t i = 0; i < length; i++) {
        total = total * 10 + (unsigned long long) (text[i] - '0');
        if (total > limit) {
            return false;
        }
    }
    *number = total;
    return true;
}

/* (G15, G40) 숫자 + K 또는 M (단위 필수), 1 이상 */
static bool parse_size_value (config_parser_t * parser, const char * key,
                              const char * text, size_t * target) {
    size_t digits = strspn(text, "0123456789");
    const char * unit = text + digits;
    unsigned long long scale = 0;
    unsigned long long number = 0;

    if (strcmp(unit, "K") == 0) {
        scale = KILOBYTE;
    } else if (strcmp(unit, "M") == 0) {
        scale = MEGABYTE;
    }
    if (scale == 0
        || !read_number(text, digits, MAX_SIZE_VALUE / scale, &number)
        || number == 0) {
        report_config_error(parser, CONFIG_INVALID,
                            "%s must be a size like 64K or 1M: %s", key,
                            text);
        return false;
    }
    *target = (size_t) (number * scale);
    return true;
}

/* (G40) */
static bool parse_switch_value (config_parser_t * parser, const char * key,
                                const char * text, bool * target) {
    if (strcmp(text, "on") == 0) {
        *target = true;
        return true;
    }
    if (strcmp(text, "off") == 0) {
        *target = false;
        return true;
    }
    report_config_error(parser, CONFIG_INVALID, "%s must be on or off: %s",
                        key, text);
    return false;
}

/* (G15) 실제 확인(realpath)은 다 읽은 뒤 open_root에서 */
static bool copy_path_value (config_parser_t * parser, const char * text,
                             char ** target) {
    *target = copy_text(text, strlen(text));
    if (*target == NULL) {
        report_config_error(parser, CONFIG_UNAVAILABLE, "out of memory");
        return false;
    }
    return true;
}

/* (G15) listen·root 필수, linger_quiet ≤ linger_total */
static bool check_required (config_parser_t * parser) {
    const setting_t * quiet = find_setting(parser, "linger_quiet");
    const setting_t * total = find_setting(parser, "linger_total");

    parser->line_number = 0;
    if (find_setting(parser, "listen")->line == 0) {
        report_config_error(parser, CONFIG_INVALID, "missing listen");
        return false;
    }
    if (find_setting(parser, "root")->line == 0) {
        report_config_error(parser, CONFIG_INVALID, "missing root");
        return false;
    }
    if (parser->config->linger_quiet_ms > parser->config->linger_total_ms) {
        parser->line_number = pick_later_line(quiet->line, total->line);
        report_config_error(parser, CONFIG_INVALID,
                            "linger_quiet must not be longer than "
                            "linger_total");
        return false;
    }
    return true;
}

/* (G15) 둘 중 늦게 나온 줄. 기본값이면 0 */
static size_t pick_later_line (size_t first, size_t second) {
    return first > second ? first : second;
}

/* (G15, G31, S26) root는 copy_static_root. 실패는 환경 문제 */
static bool open_root (config_parser_t * parser, const char * root_text) {
    parser->config->root = copy_static_root(root_text);
    if (parser->config->root == NULL) {
        parser->line_number = find_setting(parser, "root")->line;
        report_config_error(parser, CONFIG_UNAVAILABLE, "root %s: %s",
                            root_text, strerror(errno));
        return false;
    }
    return true;
}
