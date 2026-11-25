#ifndef ROUTER_H
#define ROUTER_H

#include <stddef.h>

#include "server_config.h"

/* ── 공개 함수 ── */

/* (G1~G3, G37) path: 원문 요청 경로 (쿼리 포함 가능, '\0' 없어도 됨).
 * 2단계 정리를 한 경로로 규칙을 고른다.
 * 반환: 200 = *route에 맞는 규칙, NULL = 정적.
 *       그 밖 400 / 403 / 404 = 정리 오류 (*route는 NULL) */
int find_route (const server_config_t * config, const char * path,
                size_t path_length, const route_t ** route);

#endif
