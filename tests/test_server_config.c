#include <arpa/inet.h>
#include <limits.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "server_config.h"
#include "check.h"

/* (CF) 설정 글 하나를 담는 버퍼 */
#define CONFIG_TEXT_SIZE (PATH_MAX + 1024)

static char base_directory[] = "/tmp/config_test_XXXXXX";
static char root_directory[PATH_MAX];
static char config_path[PATH_MAX];
static char error_text[CONFIG_ERROR_SIZE];

static bool create_fixture (void);
static void destroy_fixture (void);
static void test_cf1_example_file (void);
static config_result_t load_text (const char * text,
                                  server_config_t * config);
static bool write_config (const char * text);
static bool is_address (const struct sockaddr_in * address,
                        const char * ip, int port);
static void test_cf2_required_keys (void);
static void test_cf3_unknown_key (void);
static void test_cf4_time_units (void);
static void check_invalid (const char * text);
static void test_cf5_duplicates (void);
static void test_cf6_value_checks (void);
static void test_cf7_route_trailing_slash (void);

/* ── 공개 함수 ── */

int main (void) {
    if (!create_fixture()) {
        printf("테스트 디렉토리를 만들지 못함\n");
        destroy_fixture();
        return 1;
    }

    test_cf1_example_file();
    test_cf2_required_keys();
    test_cf3_unknown_key();
    test_cf4_time_units();
    test_cf5_duplicates();
    test_cf6_value_checks();
    test_cf7_route_trailing_slash();

    destroy_fixture();
    return report_checks();
}

/* ── 도우미 함수 ── */

/* (G15) root는 copy_static_root를 거치므로 실제 디렉토리가 있어야 한다 */
static bool create_fixture (void) {
    if (mkdtemp(base_directory) == NULL) {
        return false;
    }
    snprintf(root_directory, sizeof(root_directory), "%s/www",
             base_directory);
    snprintf(config_path, sizeof(config_path), "%s/server.conf",
             base_directory);
    return mkdir(root_directory, 0755) == 0;
}

/* 만든 것만 거꾸로 지운다. 없는 것은 실패해도 무시 */
static void destroy_fixture (void) {
    unlink(config_path);
    rmdir(root_directory);
    rmdir(base_directory);
}

/* CF-1 (G15) 예시 파일 + 주석·빈 줄. 적지 않은 항목은 4번 표 기본값 */
static void test_cf1_example_file (void) {
    char text[CONFIG_TEXT_SIZE];
    server_config_t config;
    char * expected_root = realpath(root_directory, NULL);

    BEGIN_TEST("CF-1");
    snprintf(text, sizeof(text),
             "# 예시\n"
             "listen 8080\n"
             "\n"
             "root %s\n"
             "# 백엔드 두 개\n"
             "route /api least_conn 127.0.0.1:8001 127.0.0.1:8002\n"
             "\n"
             "max_body 1M\n",
             root_directory);
    config_result_t result = load_text(text, &config);
    CHECK_INT(result, CONFIG_OK);
    if (result != CONFIG_OK) {
        free(expected_root);
        return;
    }
    CHECK_INT(config.port, 8080);
    CHECK(expected_root != NULL && config.root != NULL
          && strcmp(config.root, expected_root) == 0);
    CHECK_INT(config.route_count, 1);
    CHECK_INT(config.backend_count, 2);
    if (config.route_count != 1 || config.backend_count != 2) {
        destroy_server_config(&config);
        free(expected_root);
        return;
    }
    CHECK_VIEW(config.routes[0].prefix, config.routes[0].prefix_length,
               "/api");
    CHECK_INT(config.routes[0].balance, BALANCE_LEAST_CONN);
    CHECK_INT(config.routes[0].backend_count, 2);
    CHECK(is_address(&config.backends[config.routes[0].backend_indexes[0]],
                     "127.0.0.1", 8001));
    CHECK(is_address(&config.backends[config.routes[0].backend_indexes[1]],
                     "127.0.0.1", 8002));
    CHECK_INT(config.max_body, 1048576);
    CHECK_INT(config.header_timeout_ms, 60000);
    CHECK_INT(config.body_timeout_ms, 60000);
    CHECK_INT(config.send_timeout_ms, 60000);
    CHECK_INT(config.keepalive_timeout_ms, 75000);
    CHECK_INT(config.linger_quiet_ms, 5000);
    CHECK_INT(config.linger_total_ms, 30000);
    CHECK_INT(config.backend_connect_timeout_ms, 5000);
    CHECK_INT(config.backend_send_timeout_ms, 60000);
    CHECK_INT(config.backend_read_timeout_ms, 60000);
    CHECK_INT(config.backend_fail_timeout_ms, 10000);
    CHECK_INT(config.max_backend_response, 8388608);
    CHECK(config.access_log);
    CHECK(!config.count_log);
    destroy_server_config(&config);
    free(expected_root);
}

/* (G15, G31) 설정 파일에 쓰고 읽는다. 오류 문구는 error_text에 남는다 */
static config_result_t load_text (const char * text,
                                  server_config_t * config) {
    error_text[0] = '\0';
    if (!write_config(text)) {
        printf("설정 파일을 쓰지 못함\n");
        return CONFIG_UNAVAILABLE;
    }
    return load_server_config(config_path, config, error_text,
                              sizeof(error_text));
}

static bool write_config (const char * text) {
    FILE * file = fopen(config_path, "w");
    if (file == NULL) {
        return false;
    }
    bool is_written = fputs(text, file) >= 0;
    return fclose(file) == 0 && is_written;
}

static bool is_address (const struct sockaddr_in * address,
                        const char * ip, int port) {
    return address->sin_family == AF_INET
           && address->sin_addr.s_addr == inet_addr(ip)
           && address->sin_port == htons((uint16_t) port);
}

/* CF-2 (G15) 오류 문구에 무엇이 없는지 나온다 */
static void test_cf2_required_keys (void) {
    char text[CONFIG_TEXT_SIZE];
    server_config_t config;

    BEGIN_TEST("CF-2");
    snprintf(text, sizeof(text), "root %s\n", root_directory);
    CHECK_INT(load_text(text, &config), CONFIG_INVALID);
    CHECK(strstr(error_text, "listen") != NULL);

    CHECK_INT(load_text("listen 8080\n", &config), CONFIG_INVALID);
    CHECK(strstr(error_text, "root") != NULL);
}

/* CF-3 (G15) "파일:줄:"로 시작한다. 모르는 키는 3번째 줄 */
static void test_cf3_unknown_key (void) {
    char text[CONFIG_TEXT_SIZE];
    char prefix[PATH_MAX + 16];
    server_config_t config;

    BEGIN_TEST("CF-3");
    snprintf(text, sizeof(text), "listen 8080\nroot %s\nlisten_port 80\n",
             root_directory);
    snprintf(prefix, sizeof(prefix), "%s:3:", config_path);
    CHECK_INT(load_text(text, &config), CONFIG_INVALID);
    CHECK(strncmp(error_text, prefix, strlen(prefix)) == 0);
}

/* CF-4 (G15) 단위 없음 / 틀린 단위 */
static void test_cf4_time_units (void) {
    BEGIN_TEST("CF-4");
    check_invalid("header_timeout 60\n");
    check_invalid("header_timeout 60m\n");
}

/* (G15) 필수 항목 두 줄 뒤에 extra를 붙인 파일은 오류여야 한다 */
static void check_invalid (const char * extra) {
    char text[CONFIG_TEXT_SIZE];
    server_config_t config;

    snprintf(text, sizeof(text), "listen 8080\nroot %s\n%s",
             root_directory, extra);
    CHECK_INT(load_text(text, &config), CONFIG_INVALID);
    CHECK(error_text[0] != '\0');
}

/* CF-5 (G15) 같은 키 두 번 / route 두 번(다른 접두사) / 같은 접두사 두 번 */
static void test_cf5_duplicates (void) {
    char text[CONFIG_TEXT_SIZE];
    server_config_t config;

    BEGIN_TEST("CF-5");
    check_invalid("listen 8081\n");

    snprintf(text, sizeof(text),
             "listen 8080\nroot %s\n"
             "route /api round_robin 127.0.0.1:8001\n"
             "route /admin round_robin 127.0.0.1:8002\n",
             root_directory);
    config_result_t result = load_text(text, &config);
    CHECK_INT(result, CONFIG_OK);
    if (result == CONFIG_OK) {
        CHECK_INT(config.route_count, 2);
        destroy_server_config(&config);
    }

    check_invalid("route /api round_robin 127.0.0.1:8001\n"
                  "route /api round_robin 127.0.0.1:8002\n");
}

/* CF-6 (G15) 포트 0·70000, 호스트 이름, 숫자 아닌 포트, linger 순서 */
static void test_cf6_value_checks (void) {
    char text[CONFIG_TEXT_SIZE];
    server_config_t config;

    BEGIN_TEST("CF-6");
    snprintf(text, sizeof(text), "listen 0\nroot %s\n", root_directory);
    CHECK_INT(load_text(text, &config), CONFIG_INVALID);
    snprintf(text, sizeof(text), "listen 70000\nroot %s\n", root_directory);
    CHECK_INT(load_text(text, &config), CONFIG_INVALID);
    check_invalid("route /api round_robin localhost:8001\n");
    check_invalid("route /api round_robin 127.0.0.1:80o1\n");
    check_invalid("linger_quiet 40s\nlinger_total 30s\n");
}

/* CF-7 (G15) 끝 '/'는 정리한다 */
static void test_cf7_route_trailing_slash (void) {
    char text[CONFIG_TEXT_SIZE];
    server_config_t config;

    BEGIN_TEST("CF-7");
    snprintf(text, sizeof(text),
             "listen 8080\nroot %s\nroute /api/ round_robin 127.0.0.1:8001\n",
             root_directory);
    config_result_t result = load_text(text, &config);
    CHECK_INT(result, CONFIG_OK);
    if (result != CONFIG_OK) {
        return;
    }
    CHECK_INT(config.route_count, 1);
    if (config.route_count == 1) {
        CHECK_VIEW(config.routes[0].prefix, config.routes[0].prefix_length,
                   "/api");
    }
    destroy_server_config(&config);
}
