#ifndef UPSTREAM_H
#define UPSTREAM_H

#include <netinet/in.h>
#include <stdbool.h>
#include <stddef.h>

#include "server_config.h"

/* (G13, G14) 백엔드 하나의 실행 중 상태. 5단계에서 원자적으로 (B26) */
typedef struct {
    /* 처리 중 요청 수: 연결할 때 +1, release_backend에서 −1 (G14, G26) */
    size_t active;
    /* 이 시각까지 빠짐. 0 = 안 빠짐 (G13) */
    long long failed_until_ms;
} backend_state_t;

/* (G29) 설정(바뀌지 않음)과 나눈 실행 중 상태. 루프는 빌려 쓴다 */
typedef struct {
    const server_config_t * config;
    /* config->backends와 같은 순서 */
    backend_state_t * backends;
    /* 규칙마다 다음 라운드로빈 차례 (route 안 순번). config->routes와 같은 순서 */
    size_t * next_turns;
} upstream_t;

/* ── 공개 함수 ── */

/* (G29) config: 빌림 (upstream보다 오래 살아야 함). 반환: NULL = 할당 실패.
 * destroy_upstream으로 푼다 */
upstream_t * create_upstream (const server_config_t * config);

/* (G29) NULL이면 아무것도 안 한다 */
void destroy_upstream (upstream_t * upstream);

/* (G13, G14, G22) 이번 요청에서 아직 안 해 본 백엔드 중 하나를 고른다.
 * 빠지지 않은 것이 있으면 그중에서, 전부 빠져 있으면 빠짐 표시를 무시한다.
 * tried: route->backend_count개. position: route 안 순번.
 * 반환: false = 전부 해 봄 */
bool pick_backend (upstream_t * upstream, const route_t * route,
                   const bool * tried, long long now_ms, size_t * position);

/* (G15) position: route 안 순번 */
const struct sockaddr_in * find_backend_address (const upstream_t * upstream,
                                                 const route_t * route,
                                                 size_t position);

/* (G14) 처리 중 +1 (count_log면 stderr에 한 줄, G41) */
void add_backend_use (upstream_t * upstream, const route_t * route,
                      size_t position);

/* (G14, G26) 처리 중 −1 (count_log면 stderr에 한 줄, G41) */
void remove_backend_use (upstream_t * upstream, const route_t * route,
                         size_t position);

/* (G13) backend_fail_timeout 동안 뺀다 */
void mark_backend_failed (upstream_t * upstream, const route_t * route,
                          size_t position, long long now_ms);

/* (G13) 연결에 성공하면 빠짐 표시를 지운다 */
void mark_backend_alive (upstream_t * upstream, const route_t * route,
                         size_t position);

#endif
