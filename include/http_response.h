#ifndef HTTP_RESPONSE_H
#define HTTP_RESPONSE_H

#include <stdbool.h>
#include <stddef.h>
#include <time.h>

#include "http_request.h"

/* (S27) 경로는 1단계에서 MAX_HEADER_SIZE 안으로 막혀 들어온다.
 * 301에서 경로를 뺀 나머지는 290바이트 → 그보다 큰 2의 거듭제곱 512 */
#define RESPONSE_HEAD_MARGIN 512
#define MAX_RESPONSE_HEAD_SIZE (MAX_HEADER_SIZE + RESPONSE_HEAD_MARGIN)

/* (S25) 파서 오류·정적 응답·4단계 오류 응답이 같이 쓴다 */
typedef struct {
    int status;
    bool keep_alive;
    /* 200일 때만 쓴다 (MIME 표 문자열, 빌림) */
    const char * content_type;
    /* 200일 때만 쓴다 (파일 크기) */
    size_t content_length;
    /* (S22) 301일 때만. 원문 경로를 빌리고 쿼리 앞까지. 끝의 '/'는 쓸 때 붙인다 */
    const char * location;
    size_t location_length;
} response_head_t;

/* ── 공개 함수 ── */

/* (R1f, R1g, S12, S13, S19, S23, S27, S28)
 * buffer: MAX_RESPONSE_HEAD_SIZE 바이트. 200이 아니면 오류 본문까지 이어 쓴다.
 * 반환: 쓴 바이트 수 ('\0'은 쓰지 않는다) */
size_t write_response_head (const response_head_t * head, time_t now,
                            char * buffer);

#endif
