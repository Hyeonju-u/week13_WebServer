#include "upstream.h"

#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>

static backend_state_t * find_state (upstream_t * upstream,
                                     const route_t * route, size_t position);
static bool has_live_untried (upstream_t * upstream, const route_t * route,
                              const bool * tried, long long now_ms);
static bool is_live (const backend_state_t * state, long long now_ms);
static bool is_candidate (upstream_t * upstream, const route_t * route,
                          const bool * tried, size_t position,
                          bool needs_live, long long now_ms);
static bool is_better (upstream_t * upstream, const route_t * route,
                       size_t position, size_t best);
static void report_active (const upstream_t * upstream,
                           const route_t * route, size_t position);

/* ── 공개 함수 ── */

/* (G29) 할당을 다 끝낸 뒤에 채운다 */
upstream_t * create_upstream (const server_config_t * config) {
    upstream_t * upstream = malloc(sizeof(*upstream));
    backend_state_t * backends = calloc(config->backend_count + 1,
                                        sizeof(*backends));
    size_t * next_turns = calloc(config->route_count + 1,
                                 sizeof(*next_turns));

    if (upstream == NULL || backends == NULL || next_turns == NULL) {
        free(upstream);
        free(backends);
        free(next_turns);
        return NULL;
    }
    upstream->config = config;
    upstream->backends = backends;
    upstream->next_turns = next_turns;
    return upstream;
}

/* (G29) */
void destroy_upstream (upstream_t * upstream) {
    if (upstream == NULL) {
        return;
    }
    free(upstream->backends);
    free(upstream->next_turns);
    free(upstream);
}

/* (G13, G14, G22) 다음 차례부터 한 바퀴 돈다. round_robin은 처음 맞는 것,
 * least_conn은 처리 중이 가장 적은 것 (같으면 차례가 빠른 것 = 라운드로빈) */
bool pick_backend (upstream_t * upstream, const route_t * route,
                   const bool * tried, long long now_ms, size_t * position) {
    size_t route_index = (size_t) (route - upstream->config->routes);
    size_t start = upstream->next_turns[route_index];
    size_t count = route->backend_count;
    bool needs_live = has_live_untried(upstream, route, tried, now_ms);
    bool is_found = false;
    size_t best = 0;

    for (size_t i = 0; i < count; i++) {
        size_t candidate = (start + i) % count;
        if (!is_candidate(upstream, route, tried, candidate, needs_live,
                          now_ms)) {
            continue;
        }
        if (!is_found || is_better(upstream, route, candidate, best)) {
            best = candidate;
            is_found = true;
        }
    }
    if (!is_found) {
        return false;
    }
    upstream->next_turns[route_index] = (best + 1) % count;
    *position = best;
    return true;
}

/* (G15) */
const struct sockaddr_in * find_backend_address (const upstream_t * upstream,
                                                 const route_t * route,
                                                 size_t position) {
    return &upstream->config->backends[route->backend_indexes[position]];
}

/* (G14) */
void add_backend_use (upstream_t * upstream, const route_t * route,
                      size_t position) {
    find_state(upstream, route, position)->active++;
    report_active(upstream, route, position);
}

/* (G14, G26) release_backend가 fd가 있을 때만 부르므로 0 밑으로 가지 않는다 */
void remove_backend_use (upstream_t * upstream, const route_t * route,
                         size_t position) {
    backend_state_t * state = find_state(upstream, route, position);

    if (state->active > 0) {
        state->active--;
    }
    report_active(upstream, route, position);
}

/* (G13) */
void mark_backend_failed (upstream_t * upstream, const route_t * route,
                          size_t position, long long now_ms) {
    find_state(upstream, route, position)->failed_until_ms =
        now_ms + upstream->config->backend_fail_timeout_ms;
}

/* (G13) */
void mark_backend_alive (upstream_t * upstream, const route_t * route,
                         size_t position) {
    find_state(upstream, route, position)->failed_until_ms = 0;
}

/* ── 도우미 함수 ── */

/* (G29) 같은 주소는 규칙이 달라도 상태 하나 */
static backend_state_t * find_state (upstream_t * upstream,
                                     const route_t * route, size_t position) {
    return &upstream->backends[route->backend_indexes[position]];
}

/* (G13, G22) 빠지지 않았고 아직 안 해 본 백엔드가 있는지 */
static bool has_live_untried (upstream_t * upstream, const route_t * route,
                              const bool * tried, long long now_ms) {
    for (size_t i = 0; i < route->backend_count; i++) {
        if (!tried[i] && is_live(find_state(upstream, route, i), now_ms)) {
            return true;
        }
    }
    return false;
}

/* (G13) 빼는 시간이 지났으면 다시 받는다 */
static bool is_live (const backend_state_t * state, long long now_ms) {
    return state->failed_until_ms <= now_ms;
}

/* (G13, G22) needs_live: 살아 있는 후보가 있으면 빠진 것은 고르지 않는다 */
static bool is_candidate (upstream_t * upstream, const route_t * route,
                          const bool * tried, size_t position,
                          bool needs_live, long long now_ms) {
    if (tried[position]) {
        return false;
    }
    return !needs_live || is_live(find_state(upstream, route, position),
                                  now_ms);
}

/* (G14) round_robin은 처음 찾은 것이 답이라 바꾸지 않는다 */
static bool is_better (upstream_t * upstream, const route_t * route,
                       size_t position, size_t best) {
    if (route->balance != BALANCE_LEAST_CONN) {
        return false;
    }
    return find_state(upstream, route, position)->active
           < find_state(upstream, route, best)->active;
}

/* (G42, L36) "backend 127.0.0.1:8001 active=N" */
static void report_active (const upstream_t * upstream,
                           const route_t * route, size_t position) {
    char address[INET_ADDRSTRLEN];
    const struct sockaddr_in * backend = find_backend_address(upstream, route,
                                                              position);

    if (!upstream->config->count_log) {
        return;
    }
    if (inet_ntop(AF_INET, &backend->sin_addr, address,
                  sizeof(address)) == NULL) {
        return;
    }
    fprintf(stderr, "backend %s:%u active=%zu\n", address,
            (unsigned) ntohs(backend->sin_port),
            upstream->backends[route->backend_indexes[position]].active);
}
