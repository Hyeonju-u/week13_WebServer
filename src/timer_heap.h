#ifndef TIMER_HEAP_H
#define TIMER_HEAP_H

#include "connection.h"

typedef struct timer_heap timer_heap_t;

timer_heap_t * create_timer_heap (void);
void destroy_timer_heap (timer_heap_t * heap);
int set_heap_deadline (timer_heap_t * heap, conn_t * conn,
    long long deadline_ms);
void remove_from_heap (timer_heap_t * heap, conn_t * conn);
conn_t * find_earliest_conn (const timer_heap_t * heap);
long long get_now_ms (void);

#endif
