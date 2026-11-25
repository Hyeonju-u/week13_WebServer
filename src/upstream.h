#ifndef UPSTREAM_H
#define UPSTREAM_H

#include <stddef.h>

#include "proxy_config.h"

/*
 * 설정(proxy_config_t)은 시작한 뒤 바뀌지 않는다. 실행 중에 바뀌는 분배 차례와
 * 빠진 시각은 여기에만 둔다. 루프를 여러 개로 늘릴 때 나눠 쓸 곳이 한 곳이 된다.
 */
typedef struct {
    size_t next_index;
    long long * failed_until_ms;
} route_state_t;

typedef struct {
    const proxy_config_t * config;
    route_state_t * routes;
} upstream_t;

upstream_t * create_upstream (const proxy_config_t * config);
void destroy_upstream (upstream_t * upstream);
size_t take_first_backend (upstream_t * upstream, const route_t * route);
int has_live_backend (const upstream_t * upstream, const route_t * route,
    long long now_ms);
int is_backend_live (const upstream_t * upstream, const route_t * route,
    size_t index, long long now_ms);
void mark_backend_failed (upstream_t * upstream, const route_t * route,
    size_t index, long long now_ms);
void mark_backend_alive (upstream_t * upstream, const route_t * route,
    size_t index);

#endif
