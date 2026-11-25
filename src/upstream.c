#include "upstream.h"

#include <stdlib.h>

static route_state_t * create_route_states (const proxy_config_t * config);
static void init_route_state (route_state_t * state, size_t backend_count);
static void destroy_route_states (route_state_t * routes, size_t count);
static route_state_t * find_route_state (const upstream_t * upstream,
    const route_t * route);

/* ── 공개 함수 ── */

/* 할당을 다 끝낸 뒤에 채운다. 실패하면 NULL */
upstream_t * create_upstream (const proxy_config_t * config) {
    upstream_t * upstream = malloc(sizeof(*upstream));
    route_state_t * routes = create_route_states(config);
    if (upstream == NULL || routes == NULL) {
        free(upstream);
        destroy_route_states(routes, config->route_count);
        return NULL;
    }
    upstream->config = config;
    upstream->routes = routes;
    return upstream;
}

void destroy_upstream (upstream_t * upstream) {
    if (upstream == NULL) {
        return;
    }
    destroy_route_states(upstream->routes, upstream->config->route_count);
    free(upstream);
}

/*
 * 이번 요청이 처음 시도할 백엔드 위치를 주고 차례를 하나 넘긴다 (Round-Robin).
 * 번호표를 원자적으로 받아서 루프가 달라도 0, 1, 2, 0, ... 순서가 된다 (M5)
 */
size_t take_first_backend (upstream_t * upstream, const route_t * route) {
    route_state_t * state = find_route_state(upstream, route);
    size_t turn = atomic_fetch_add(&state->next_index, 1);
    return turn % route->backend_count;
}

/* 하나도 없으면 호출자는 빠진 표시를 무시하고 백엔드마다 한 번씩 시도한다.
 * 잠깐 모두 실패했을 때 빼는 시간 내내 502만 나가는 것을 막는다 (#2 G22) */
int has_live_backend (const upstream_t * upstream, const route_t * route,
    long long now_ms) {
    for (size_t i = 0; i < route->backend_count; i++) {
        if (is_backend_live(upstream, route, i, now_ms)) {
            return 1;
        }
    }
    return 0;
}

/* 빼는 시간이 지났으면 다시 후보다 (#2 G13). 다른 루프가 뺀 것도 보인다 (M5) */
int is_backend_live (const upstream_t * upstream, const route_t * route,
    size_t index, long long now_ms) {
    atomic_llong * until = &find_route_state(upstream, route)
        ->failed_until_ms[index];
    return atomic_load(until) <= now_ms;
}

/* 연결 거절·연결 타임아웃·응답 타임아웃일 때만 부른다. 응답 형식 문제는
 * 그 요청 하나의 문제라 백엔드 전체를 빼지 않는다 (#2 G41) */
void mark_backend_failed (upstream_t * upstream, const route_t * route,
    size_t index, long long now_ms) {
    long long fail_ms =
        (long long) upstream->config->backend_fail_timeout_seconds * 1000;
    atomic_store(&find_route_state(upstream, route)->failed_until_ms[index],
        now_ms + fail_ms);
}

/* 표시를 무시하고 시도한 백엔드가 살아났으면 다음 요청부터 바로 받게 한다 */
void mark_backend_alive (upstream_t * upstream, const route_t * route,
    size_t index) {
    atomic_store(&find_route_state(upstream, route)->failed_until_ms[index],
        0);
}

/* ── 도우미 함수 ── */

/* 규칙이 없으면 빈 배열(NULL 아님)을 돌려준다. 실패하면 NULL */
static route_state_t * create_route_states (const proxy_config_t * config) {
    route_state_t * routes = calloc(config->route_count + 1, sizeof(*routes));
    if (routes == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < config->route_count; i++) {
        routes[i].failed_until_ms = calloc(config->routes[i].backend_count,
            sizeof(*routes[i].failed_until_ms));
        if (routes[i].failed_until_ms == NULL) {
            destroy_route_states(routes, i);
            return NULL;
        }
        init_route_state(&routes[i], config->routes[i].backend_count);
    }
    return routes;
}

/* calloc의 0바이트가 원자 변수의 0이라는 보장은 표준에 없어서 직접 초기화한다 */
static void init_route_state (route_state_t * state, size_t backend_count) {
    atomic_init(&state->next_index, 0);
    for (size_t i = 0; i < backend_count; i++) {
        atomic_init(&state->failed_until_ms[i], 0);
    }
}

static void destroy_route_states (route_state_t * routes, size_t count) {
    if (routes == NULL) {
        return;
    }
    for (size_t i = 0; i < count; i++) {
        free(routes[i].failed_until_ms);
    }
    free(routes);
}

/* route는 config->routes 배열 안을 가리키므로 위치로 상태를 찾는다 */
static route_state_t * find_route_state (const upstream_t * upstream,
    const route_t * route) {
    return &upstream->routes[route - upstream->config->routes];
}
