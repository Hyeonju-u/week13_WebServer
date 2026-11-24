#include "timer_list.h"

#include <stddef.h>
#include <time.h>

static void append_node (timer_list_t * list, timer_node_t * node);

/* ── 공개 함수 ── */

/* (L29) 벽시계가 바뀌어도 타임아웃이 흔들리지 않게 */
long long read_clock_ms (void) {
    struct timespec now;

    clock_gettime(CLOCK_MONOTONIC, &now);
    return (long long) now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

/* (L29) 갱신은 빼서 맨 뒤로 → O(1) */
void start_timer (timer_list_t * list, timer_node_t * node, long long now_ms) {
    stop_timer(node);
    node->deadline_ms = now_ms + list->duration_ms;
    append_node(list, node);
}

/* (L29) */
void stop_timer (timer_node_t * node) {
    timer_list_t * list = node->list;

    if (list == NULL) {
        return;
    }
    if (node->previous == NULL) {
        list->first = node->next;
    } else {
        node->previous->next = node->next;
    }
    if (node->next == NULL) {
        list->last = node->previous;
    } else {
        node->next->previous = node->previous;
    }
    node->previous = NULL;
    node->next = NULL;
    node->list = NULL;
}

/* (L29) */
timer_node_t * find_expired_timer (const timer_list_t * list,
                                   long long now_ms) {
    if (list->first == NULL || list->first->deadline_ms > now_ms) {
        return NULL;
    }
    return list->first;
}

/* (L29) */
long long find_time_until_first (const timer_list_t * list, long long now_ms) {
    if (list->first == NULL) {
        return -1;
    }
    if (list->first->deadline_ms <= now_ms) {
        return 0;
    }
    return list->first->deadline_ms - now_ms;
}

/* (L23) */
bool is_timer_running (const timer_node_t * node) {
    return node->list != NULL;
}

/* ── 도우미 함수 ── */

/* (L29) */
static void append_node (timer_list_t * list, timer_node_t * node) {
    node->list = list;
    node->next = NULL;
    node->previous = list->last;
    if (list->last == NULL) {
        list->first = node;
    } else {
        list->last->next = node;
    }
    list->last = node;
}
