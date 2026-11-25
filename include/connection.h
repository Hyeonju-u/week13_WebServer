#ifndef CONNECTION_H
#define CONNECTION_H

#include <netinet/in.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "event_loop.h"
#include "http_request.h"
#include "proxy_response.h"
#include "server_config.h"
#include "static_file.h"
#include "timer_list.h"

/* (G25) epoll data.ptr이 가리키는 구조체의 첫 필드. 손님 fd는 카드,
 * 백엔드 fd는 카드 안의 proxy로 등록한다 */
typedef enum {
    ENDPOINT_CLIENT,
    ENDPOINT_BACKEND
} endpoint_kind_t;

/* (L24, G26) 전환표는 docs/step3-loop.md 2번 + 4단계 결과 보고 */
typedef enum {
    CONN_READ_HEAD,
    CONN_SEND,
    CONN_DISCARD,
    CONN_IDLE,
    CONN_LINGER,
    CONN_CONTINUE,
    CONN_READ_BODY,
    CONN_CONNECT,
    CONN_BACKEND_SEND,
    CONN_BACKEND_READ
} conn_state_t;

struct conn;

/* (G13, G14, G26~G28) 카드 안의 백엔드 쪽. 요청 하나 동안만 쓴다 */
typedef struct {
    /* (G25) 항상 ENDPOINT_BACKEND. 첫 필드여야 한다 */
    endpoint_kind_t kind;
    struct conn * owner;
    /* NULL = 프록시 요청이 아님 */
    const route_t * route;
    /* (G26) -1 = 백엔드 연결 없음 */
    int fd;
    /* 지금 시도 중인 백엔드 (route 안 순번) */
    size_t position;
    /* (G13, G22) 소유. route->backend_count개, 이번 요청에서 시도했는지 */
    bool * tried;
    size_t attempts;
    /* (G28) 소유. 고친 요청 헤더. 재시도에도 그대로 쓴다 */
    char * request_head;
    size_t request_head_length;
    size_t request_head_sent;
    size_t body_sent;
    /* (G26, G27) 소유. 받는 중인 백엔드 응답 (헤더 + 본문) */
    char * response;
    size_t response_length;
    size_t response_capacity;
    bool has_head;
    backend_head_t head;
    /* (G18) 첫 connect부터 백엔드를 놓을 때까지 */
    long long started_ms;
    long long finished_ms;
    /* (G12, G36) 마지막 connect 실패의 응답 코드 */
    int failure_status;
    /* (G39) NULL = 실패 없음 */
    const char * cause;
} proxy_t;

/* (R3c, L24) 진행 카드. 연결마다 하나 */
typedef struct conn {
    /* (G25) 항상 ENDPOINT_CLIENT. 첫 필드여야 한다 */
    endpoint_kind_t kind;
    /* (G25) 정리됨. 묶음이 끝나면 free된다 */
    bool closed;
    struct conn * next_closed;
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
    /* (G40, G46) 소유. 메모리 본문 (백엔드 응답 버퍼를 넘겨받음). NULL = 파일 */
    char * response_body;
    size_t response_body_start;
    /* (L9, L16, G40) 약속한 본문 길이와 보낸 위치 (파일 또는 메모리) */
    size_t body_length;
    size_t body_sent;
    size_t bytes_sent;
    /* (G7) 100 Continue 줄을 보낸 바이트 */
    size_t continue_sent;
    /* (L37) 응답 끝 로그의 꼬리표를 바꿀 때 ("timeout") */
    const char * finish_tag;
    proxy_t proxy;
} conn_t;

/* ── 공개 함수 ── */

/* (L28) fd: 성공하면 넘겨받는다 (destroy_conn이 닫음). 실패하면 호출자가 닫는다.
 * 반환: NULL = 할당 실패 */
conn_t * create_conn (int fd, const struct sockaddr_in * peer);

/* (L1, L24) epoll에 넣고 헤더 타이머를 건다. 반환: false = 실패 (호출자가 정리) */
bool start_conn (loop_t * loop, conn_t * conn);

/* (L3, L24) 손님 fd에서 온 이벤트. 정리되면 *slot이 NULL이 된다 */
void handle_conn_event (loop_t * loop, conn_t ** slot, uint32_t events);

/* (L5, L6, L11, L12, L26, G30) 카드 타이머가 마감됨. 정리되면 *slot이 NULL */
void expire_conn (loop_t * loop, conn_t ** slot);

/* (L28, G25, G26) 연결 정리는 이 함수 하나: 손님 fd, 백엔드(fd·처리 중 수·
 * 응답 버퍼), 요청·응답 버퍼. 두 번 불러도 안전 (*slot == NULL이거나
 * 이미 closed면 그냥 돌아감). 카드는 free_closed_conns가 푼다.
 * tag: 진행 중인 요청이 있으면 로그 꼬리표 (L37), NULL 가능 */
void destroy_conn (loop_t * loop, conn_t ** slot, const char * tag);

/* (G25) epoll_wait 묶음과 타이머 처리가 끝난 뒤에 부른다 */
void free_closed_conns (loop_t * loop);

/* (G12, G34, G36) 우리가 만든 502·504·500 응답. 본문은 이미 다 받았다.
 * 500은 닫고(L10), 나머지는 요청의 keep-alive를 따른다 */
void respond_with_proxy_status (loop_t * loop, conn_t ** slot, int status);

/* (G10, G40, G46) loop->head에 쓴 head_length바이트 헤더와 메모리 본문을 보낸다.
 * body: 넘겨받는다 (다 보내거나 정리할 때 free). 본문은 body + body_start부터.
 * 연결 유지는 요청의 keep-alive를 따른다 (G34) */
void start_proxy_response (loop_t * loop, conn_t ** slot, size_t head_length,
                           char * body, size_t body_start,
                           size_t body_length, int status);

/* (G21) 백엔드를 기다리는 동안 손님 fd 관심 이벤트를 바꾼다.
 * 반환: false = epoll 실패 (호출자가 정리) */
bool watch_client (loop_t * loop, conn_t * conn, uint32_t events);

#endif
