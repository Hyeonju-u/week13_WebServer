#ifndef STATIC_FILE_H
#define STATIC_FILE_H

#include <stddef.h>

#include "http_request.h"
#include "http_response.h"

/* (B4, S2, S6) 1단계가 경로를 MAX_HEADER_SIZE 안으로 막아 준다.
 * 푼 경로와 정리한 경로는 원문보다 길어지지 않는다 ('\0' 자리 +1) */
#define MAX_PATH_TEXT_SIZE (MAX_HEADER_SIZE + 1)

/* (S25, S11) */
typedef struct {
    response_head_t head;
    /* 200일 때만 열려 있다. 그 밖 -1 */
    int fd;
} static_result_t;

/* ── 공개 함수 ── */

/* (R1d, S26) 시작할 때 한 번 부른다. 반환: realpath한 루트, NULL = 실패.
 * 돌려받은 쪽이 free */
char * copy_static_root (const char * path);

/* (R1d, R1g, R4c, S1~S11, S20, S21, S24) root: copy_static_root 결과 (빌림).
 * 맨 먼저 result->fd를 -1로 둔다. 결과는 result->head.status로 본다.
 * result는 request보다 오래 살면 안 된다 (Location이 request 경로를 빌림) */
void open_static_file (const request_t * request, const char * root,
                       static_result_t * result);

/* (S1~S6, S24, G3, G32) 원문 경로를 ? 자르기 → % 풀기 → . / .. 정리.
 * path: '\0'으로 끝나지 않아도 된다. normalized: MAX_PATH_TEXT_SIZE.
 * 반환: 200 = normalized에 '\0'으로 끝나는 경로, 그 밖 400 / 403 / 404 */
int normalize_request_path (const char * path, size_t length,
                            char * normalized);

/* (R1e, S14~S17) path: '\0'으로 끝나지 않아도 된다.
 * 반환: MIME 표 문자열 (free하지 않음) */
const char * find_mime_type (const char * path, size_t length);

/* (S11, 수명 규칙) open_static_file을 한 번 부른 뒤에만 쓴다. 두 번 불러도 안전 */
void close_static_file (static_result_t * result);

#endif
