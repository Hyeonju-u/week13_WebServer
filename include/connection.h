#ifndef CONNECTION_H
#define CONNECTION_H

#include <netinet/in.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "event_loop.h"
#include "http_request.h"
#include "static_file.h"
#include "timer_list.h"

/* (L24) 전환표는 docs/step3-loop.md 2번 */
typedef enum {
    CONN_READ_HEAD,
    CONN_SEND,
    CONN_DISCARD,
    CONN_IDLE,
    CONN_LINGER
} conn_state_t;

/* (R3c, L24) 진행 카드. 연결마다 하나 */
typedef struct {
    int fd;
    struct sockaddr_in peer;
    conn_state_t state;
    timer_node_t timer;
    /* (L29) DISCARD·LINGER의 전체 상한 */
    long long total_deadline_ms;
    /* (P11 0번) 연결 버퍼. 크기 고정, 늘리지 않는다 */
    char buffer[MAX_HEADER_SIZE];
    size_t buffer_length;
    /* (L26, L37) 지금 요청에서 1바이트라도 받았는지 */
    bool has_received;
    /* (L15) 지금 요청의 로그 줄을 이미 찍었는지 */
    bool is_logged;
    /* (B31, L10, L11) 응답 뒤 lingering으로 닫는다 */
    bool close_after;
    long long request_started_ms;
    request_t request;
    /* (L37) request.head에 파싱된 요청 라인이 있는지 */
    bool has_parsed_request;
    static_result_t result;
    int response_status;
    /* (L32) 소유. 다 보내면 free */
    char * response_head;
    size_t response_head_length;
    size_t response_head_sent;
    /* (L9, L16) 약속한 파일 길이와 보낸 위치 */
    size_t file_length;
    size_t file_sent;
    size_t bytes_sent;
    /* (L37) 응답 끝 로그의 꼬리표를 바꿀 때 ("timeout") */
    const char * finish_tag;
} conn_t;

/* ── 공개 함수 ── */

/* (L28) fd: 성공하면 넘겨받는다 (destroy_conn이 닫음). 실패하면 호출자가 닫는다.
 * 반환: NULL = 할당 실패 */
conn_t * create_conn (int fd, const struct sockaddr_in * peer);

/* (L1, L24) epoll에 넣고 헤더 타이머를 건다. 반환: false = 실패 (호출자가 정리) */
bool start_conn (loop_t * loop, conn_t * conn);

/* (L3, L24) epoll이 알려 준 이벤트. 정리되면 *slot이 NULL이 된다 */
void handle_conn_event (loop_t * loop, conn_t ** slot, uint32_t events);

/* (L5, L6, L11, L12, L26) 카드 타이머가 마감됨. 정리되면 *slot이 NULL */
void expire_conn (loop_t * loop, conn_t ** slot);

/* (L28) 연결 정리는 이 함수 하나. 두 번 불러도 안전 (*slot == NULL이면 그냥 돌아감).
 * tag: 진행 중인 요청이 있으면 로그 꼬리표 (L37), NULL 가능 */
void destroy_conn (loop_t * loop, conn_t ** slot, const char * tag);

#endif
