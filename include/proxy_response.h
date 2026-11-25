#ifndef PROXY_RESPONSE_H
#define PROXY_RESPONSE_H

#include <stdbool.h>
#include <stddef.h>

/* (G8, G9, G12, G27, G35) */
typedef enum {
    BACKEND_HEAD_NEED_MORE,
    BACKEND_HEAD_OK,
    /* 1xx (101 빼고): 이 응답을 버리고 다음 응답을 기다린다 (G8) */
    BACKEND_HEAD_INTERIM,
    /* 상태 줄·헤더 형식 깨짐, 헤더 8KB 넘음, 101 (G12, G27, G35) */
    BACKEND_HEAD_BAD,
    /* Transfer-Encoding이 있음 (G8) */
    BACKEND_HEAD_CHUNKED,
    /* Content-Length가 상한을 넘음 (G9) */
    BACKEND_HEAD_TOO_LARGE
} backend_head_result;

/* (G8) 응답 버퍼 안의 위치로 적는다 (버퍼는 realloc으로 옮겨질 수 있음) */
typedef struct {
    int status;
    /* 마지막 빈 줄까지 포함한 길이 */
    size_t head_length;
    /* 상태 줄 끝(\r\n 앞) */
    size_t status_line_end;
    /* 헤더 줄 영역 (P22와 같은 뜻) */
    size_t headers_start;
    size_t headers_length;
    bool has_content_length;
    size_t content_length;
    /* 204·304는 본문이 없다 (G8) */
    bool has_body;
} backend_head_t;

/* ── 공개 함수 ── */

/* (G8, G9, G12, G27, G35) data 앞부분의 응답 헤더를 본다.
 * find_header_end를 써서 헤더는 MAX_HEADER_SIZE까지만 받는다.
 * BACKEND_HEAD_OK·INTERIM일 때만 head를 채운다 (INTERIM은 head_length만) */
backend_head_result parse_backend_head (const char * data, size_t length,
                                        size_t max_body,
                                        backend_head_t * head);

/* (G10, G35, G44) 손님에게 보낼 응답 헤더를 buffer에 쓴다.
 * HTTP/1.1 + 상태·사유 그대로, hop-by-hop·Content-Length를 빼고 나머지는
 * 그대로, 본문이 있으면 받은 길이로 Content-Length, Connection은 손님 기준.
 * buffer: MAX_RESPONSE_HEAD_SIZE (백엔드 헤더 8,192 + 붙이는 줄 62바이트).
 * 반환: 쓴 길이 */
size_t write_client_head (const char * data, const backend_head_t * head,
                          size_t body_length, bool keep_alive,
                          char * buffer);

#endif
