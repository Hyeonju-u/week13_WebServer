#ifndef TIMER_LIST_H
#define TIMER_LIST_H

#include <stdbool.h>

/* (L29) 종류마다 리스트 하나. 같은 리스트의 기간은 모두 같아서
 * 뒤에 붙이기만 해도 마감 순서가 지켜진다 */
struct timer_list;

typedef struct timer_node {
    struct timer_node * previous;
    struct timer_node * next;
    /* NULL = 어느 리스트에도 없음 */
    struct timer_list * list;
    long long deadline_ms;
    /* 마감된 노드를 가진 쪽 (빌림) */
    void * owner;
} timer_node_t;

typedef struct timer_list {
    timer_node_t * first;
    timer_node_t * last;
    long long duration_ms;
} timer_list_t;

/* ── 공개 함수 ── */

/* (L29) CLOCK_MONOTONIC을 ms로 */
long long read_clock_ms (void);

/* (L29) 다른 리스트에 있으면 빼고 이 리스트 맨 뒤에 붙인다.
 * 마감 = now_ms + list->duration_ms */
void start_timer (timer_list_t * list, timer_node_t * node, long long now_ms);

/* (L29) 리스트에 없으면 아무것도 안 한다 */
void stop_timer (timer_node_t * node);

/* (L29) 반환: 마감이 지난 맨 앞 노드, NULL = 없음 (노드는 그대로 둔다) */
timer_node_t * find_expired_timer (const timer_list_t * list,
                                   long long now_ms);

/* (L29) 반환: 맨 앞 마감까지 남은 ms (0 이상), -1 = 리스트가 빔 */
long long find_time_until_first (const timer_list_t * list, long long now_ms);

/* (L23) */
bool is_timer_running (const timer_node_t * node);

#endif
