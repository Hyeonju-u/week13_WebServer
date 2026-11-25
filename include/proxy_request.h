#ifndef PROXY_REQUEST_H
#define PROXY_REQUEST_H

#include <netinet/in.h>
#include <stdbool.h>
#include <stddef.h>

#include "http_request.h"

/* ── 공개 함수 ── */

/* (G3~G6, G28) 백엔드로 보낼 요청 헤더를 buffer에 쓴다 ('\0' 없음).
 * 원문 경로 + HTTP/1.0, hop-by-hop·Expect를 지우고 X-Forwarded-For를 합침.
 * buffer: MAX_HEADER_SIZE 바이트. client: 손님 주소 (XFF 끝에 붙임).
 * 반환: 쓴 길이, 0 = 고친 헤더가 MAX_HEADER_SIZE를 넘음 (431) */
size_t write_backend_request (const request_t * request,
                              const struct sockaddr_in * client,
                              char * buffer);

/* (G6, G10) RFC 9110 §7.6.1의 hop-by-hop이거나, 같은 헤더 영역의
 * Connection 값에 이름이 적힌 헤더인지. headers: 헤더 줄 영역 (P22) */
bool is_hop_by_hop_header (const char * headers, size_t headers_length,
                           const header_view_t * line);

#endif
