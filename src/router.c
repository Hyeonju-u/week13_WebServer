#include "router.h"

#include <stdbool.h>
#include <string.h>

#include "static_file.h"

static const route_t * find_matching_route (const server_config_t * config,
                                            const char * normalized);
static bool is_prefix_match (const route_t * route, const char * normalized);

/* ── 공개 함수 ── */

/* (G1, G3, G37) 정리 오류는 경로와 상관없이 라우팅 전에 그 코드로 */
int find_route (const server_config_t * config, const char * path,
                size_t path_length, const route_t ** route) {
    char normalized[MAX_PATH_TEXT_SIZE];

    *route = NULL;
    int status = normalize_request_path(path, path_length, normalized);
    if (status != 200) {
        return status;
    }
    *route = find_matching_route(config, normalized);
    return 200;
}

/* ── 도우미 함수 ── */

/* (G2) 여러 개 맞으면 먼저 적힌 규칙. 안 맞으면 NULL (정적) */
static const route_t * find_matching_route (const server_config_t * config,
                                            const char * normalized) {
    for (size_t i = 0; i < config->route_count; i++) {
        if (is_prefix_match(&config->routes[i], normalized)) {
            return &config->routes[i];
        }
    }
    return NULL;
}

/* (G2, G40) 경로 조각 경계 접두사: /api는 /api, /api/x에 맞고 /apix에 안 맞음.
 * 접두사 "/"는 모든 경로에 맞는다 */
static bool is_prefix_match (const route_t * route, const char * normalized) {
    size_t length = route->prefix_length;

    if (strncmp(normalized, route->prefix, length) != 0) {
        return false;
    }
    if (length == 1) {
        return true;
    }
    return normalized[length] == '\0' || normalized[length] == '/';
}
