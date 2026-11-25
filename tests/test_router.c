#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "router.h"
#include "server_config.h"
#include "check.h"

static char api_prefix[] = "/api";
static char admin_prefix[] = "/api/admin";

static route_t create_route (char * prefix);
static void test_rt1_segment_boundary (void);
static void check_route (const server_config_t * config, const char * path,
                         int expected_status, const route_t * expected);
static void test_rt2_first_rule_wins (void);
static void test_rt3_normalized_path (void);
static void test_rt4_errors_before_routing (void);

/* ── 공개 함수 ── */

int main (void) {
    test_rt1_segment_boundary();
    test_rt2_first_rule_wins();
    test_rt3_normalized_path();
    test_rt4_errors_before_routing();
    return report_checks();
}

/* ── 도우미 함수 ── */

/* 라우팅은 접두사만 본다. 백엔드 목록은 비워 둔다 */
static route_t create_route (char * prefix) {
    route_t route = {0};

    route.prefix = prefix;
    route.prefix_length = strlen(prefix);
    route.balance = BALANCE_ROUND_ROBIN;
    return route;
}

/* RT-1 (G2) /api, /api/, /api/x → 백엔드 / /apix, / → 정적 */
static void test_rt1_segment_boundary (void) {
    route_t routes[] = { create_route(api_prefix) };
    server_config_t config = { .routes = routes, .route_count = 1 };

    BEGIN_TEST("RT-1");
    check_route(&config, "/api", 200, &routes[0]);
    check_route(&config, "/api/", 200, &routes[0]);
    check_route(&config, "/api/x", 200, &routes[0]);
    check_route(&config, "/apix", 200, NULL);
    check_route(&config, "/", 200, NULL);
}

/* (G1~G3) expected: 맞아야 하는 규칙, NULL = 정적 */
static void check_route (const server_config_t * config, const char * path,
                         int expected_status, const route_t * expected) {
    const route_t * route = &config->routes[0];
    int status = find_route(config, path, strlen(path), &route);

    CHECK_INT(status, expected_status);
    if (route != expected) {
        printf("  경로 %s\n", path);
    }
    CHECK(route == expected);
}

/* RT-2 (G2) 두 규칙이 다 맞으면 먼저 적힌 규칙. 순서를 바꿔 두 번 */
static void test_rt2_first_rule_wins (void) {
    route_t admin_first[] = {
        create_route(admin_prefix), create_route(api_prefix),
    };
    route_t api_first[] = {
        create_route(api_prefix), create_route(admin_prefix),
    };
    server_config_t admin_config = { .routes = admin_first, .route_count = 2 };
    server_config_t api_config = { .routes = api_first, .route_count = 2 };

    BEGIN_TEST("RT-2");
    check_route(&admin_config, "/api/admin/x", 200, &admin_first[0]);
    check_route(&api_config, "/api/admin/x", 200, &api_first[0]);
}

/* RT-3 (G3) 정리한 경로로 고른다: /%61pi/x → 백엔드,
 * /api/../a.html, /api/%2e%2e/a.html → 정적 */
static void test_rt3_normalized_path (void) {
    route_t routes[] = { create_route(api_prefix) };
    server_config_t config = { .routes = routes, .route_count = 1 };

    BEGIN_TEST("RT-3");
    check_route(&config, "/%61pi/x", 200, &routes[0]);
    check_route(&config, "/api/../a.html", 200, NULL);
    check_route(&config, "/api/%2e%2e/a.html", 200, NULL);
}

/* RT-4 (G3) 정리 오류는 라우팅 전에 그 코드로: /../x 403, /api/%zz 400 */
static void test_rt4_errors_before_routing (void) {
    route_t routes[] = { create_route(api_prefix) };
    server_config_t config = { .routes = routes, .route_count = 1 };

    BEGIN_TEST("RT-4");
    check_route(&config, "/../x", 403, NULL);
    check_route(&config, "/api/%zz", 400, NULL);
}
