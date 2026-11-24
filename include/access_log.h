#ifndef ACCESS_LOG_H
#define ACCESS_LOG_H

#include <netinet/in.h>
#include <stddef.h>

/* (L15, L37) 접근 로그 한 줄에 들어가는 것 */
typedef struct {
    const struct sockaddr_in * peer;
    /* NULL = 파싱 전이거나 파서 BAD → "-" (L37) */
    const char * request_line;
    size_t request_line_length;
    /* 0 = 응답 없음 → "-" (L37) */
    int status;
    size_t bytes_sent;
    long long elapsed_ms;
    /* NULL = 꼬리표 없음. "close" / "reset" / "timeout" */
    const char * tag;
} access_entry_t;

/* ── 공개 함수 ── */

/* (L15, L37) stdout에 한 줄. 줄 단위 버퍼는 main이 정한다 */
void write_access_log (const access_entry_t * entry);

#endif
