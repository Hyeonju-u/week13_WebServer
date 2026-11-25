#ifndef BACKEND_CONN_H
#define BACKEND_CONN_H

#include <stdint.h>

#include "connection.h"
#include "event_loop.h"
#include "server_config.h"

/* ── 공개 함수 ── */

/* (G26, G28) 요청 하나 몫의 백엔드 준비: 고친 요청 헤더를 만들고 시도 표시를
 * 할당한다. 실패하면 할당한 것을 정리하고 conn->proxy는 그대로 둔다.
 * 반환: 200 = 준비됨, 431 = 고친 헤더가 8KB 넘음 (G28), 500 = 할당 실패 */
int create_proxy (loop_t * loop, conn_t * conn, const route_t * route);

/* (G26) create_proxy가 할당한 것을 푼다. 백엔드 연결은 release_backend로
 * 먼저 놓는다. 두 번 불러도 안전 */
void destroy_proxy (proxy_t * proxy);

/* (G11, G13, G21) 본문을 다 받은 뒤 부른다. 손님 fd는 끊김만 보고
 * 백엔드를 골라 connect를 시작한다. 정리되면 *slot이 NULL */
void start_backend (loop_t * loop, conn_t ** slot);

/* (G8, G11, G30) 백엔드 fd에서 온 이벤트. 정리되면 *slot이 NULL */
void handle_backend_event (loop_t * loop, conn_t ** slot, uint32_t events);

/* (G11, G12, G13) CONNECT·BACKEND_SEND·BACKEND_READ 타이머가 마감됨 */
void expire_backend (loop_t * loop, conn_t ** slot);

/* (G14, G26) 백엔드 fd 닫기 + 처리 중 −1 + 받는 중인 응답 버퍼 free.
 * 백엔드 fd가 −1이면 아무것도 안 한다 (두 번 불려도 한 번만 줄어듦) */
void release_backend (loop_t * loop, conn_t * conn);

#endif
