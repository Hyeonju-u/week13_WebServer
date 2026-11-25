#ifndef EVENT_LOOP_H
#define EVENT_LOOP_H

#include <netinet/in.h>
#include <stdbool.h>
#include <stddef.h>

#include "http_response.h"
#include "server_config.h"
#include "timer_list.h"
#include "upstream.h"

/* (L9, S11) 파일 조각 버퍼. 본문 버리기·lingering 읽기도 같이 쓴다 (L33) */
#define FILE_CHUNK_SIZE 8192

/* (G25) 해제 대기 목록에 들어가는 카드 (connection.h) */
struct conn;

/* (L1) 루프 하나. 5단계에서 루프 N개가 되면 하나씩 갖는다 */
typedef struct {
    const server_config_t * config;
    /* (G29) 백엔드 실행 중 상태 (빌림) */
    upstream_t * upstream;
    int epoll_fd;
    int listen_fd;
    /* (L8) listen 소켓이 지금 epoll에 들어 있는지 */
    bool is_listening;
    size_t connection_count;
    size_t max_connections;
    /* (L29) 종류별 타이머 */
    timer_list_t header_timers;
    timer_list_t send_timers;
    timer_list_t idle_timers;
    timer_list_t quiet_timers;
    /* (G30) 본문 사이, 백엔드 connect·쓰기 사이·읽기 사이 */
    timer_list_t body_timers;
    timer_list_t connect_timers;
    timer_list_t backend_send_timers;
    timer_list_t backend_read_timers;
    /* (L23) listen 다시 넣기 */
    timer_list_t retry_timers;
    timer_node_t retry_timer;
    /* (G25) 정리했지만 아직 free하지 않은 카드. 묶음 처리가 끝나면 푼다 */
    struct conn * closed_conns;
    /* (L19, L36) */
    unsigned long long created_count;
    unsigned long long destroyed_count;
    /* (L9, L33) */
    char chunk[FILE_CHUNK_SIZE];
    /* (L32) 응답 헤더를 쓰는 자리. 실제 길이만 카드로 복사한다 */
    char head[MAX_RESPONSE_HEAD_SIZE];
} loop_t;

/* ── 공개 함수 ── */

/* (L1, L8, L22, G29) config·upstream·listen_fd는 빌림.
 * listen_fd: non-blocking으로 listen 중인 소켓.
 * 시작 줄(최대 연결 수)을 stderr에 찍는다. 반환: NULL = 실패 (stderr에 이유) */
loop_t * create_loop (const server_config_t * config, upstream_t * upstream,
                      int listen_fd);

/* (R1a, L3, G25) 끝나지 않는다. 돌아오면 epoll_wait 실패 (stderr에 이유) */
void run_loop (loop_t * loop);

/* (L1) 받은 fd를 등록하는 입구. 5단계에서 큐 + 벨(D6)로 바뀐다.
 * fd: 넘겨받는다. 실패하면 이 함수가 닫는다 */
void register_connection (loop_t * loop, int fd,
                          const struct sockaddr_in * peer);

/* (L8, L23) 카드 하나가 정리될 때 destroy_conn이 부른다 */
void release_connection_slot (loop_t * loop);

#endif
