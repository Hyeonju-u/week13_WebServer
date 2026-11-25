#include <assert.h>
#include <stdio.h>

#include "timer_heap.h"

#define COUNT 200

static void test_order_and_update (void) {
    timer_heap_t * heap = create_timer_heap();
    conn_t * conns[COUNT];
    for (int i = 0; i < COUNT; i++) {
        conns[i] = create_connection(-1, CONN_CLIENT);
        assert(set_heap_deadline(heap, conns[i],
            (long long) ((i * 37) % COUNT) + 1000) == 0);
    }
    assert(find_earliest_conn(heap)->deadline_ms == 1000);
    set_heap_deadline(heap, conns[0], 5);
    assert(find_earliest_conn(heap) == conns[0]);
    set_heap_deadline(heap, conns[0], 99999);
    assert(find_earliest_conn(heap)->deadline_ms == 1001);
    long long previous = 0;
    for (int i = 0; i < COUNT; i++) {
        conn_t * earliest = find_earliest_conn(heap);
        assert(earliest != NULL);
        assert(earliest->deadline_ms >= previous);
        previous = earliest->deadline_ms;
        remove_from_heap(heap, earliest);
        assert(earliest->heap_index == -1);
    }
    assert(find_earliest_conn(heap) == NULL);
    for (int i = 0; i < COUNT; i++) {
        destroy_connection(conns[i]);
    }
    destroy_timer_heap(heap);
}

static void test_remove_from_middle (void) {
    timer_heap_t * heap = create_timer_heap();
    conn_t * conns[10];
    for (int i = 0; i < 10; i++) {
        conns[i] = create_connection(-1, CONN_CLIENT);
        set_heap_deadline(heap, conns[i], 100 + i);
    }
    remove_from_heap(heap, conns[4]);
    remove_from_heap(heap, conns[4]);
    remove_from_heap(heap, conns[0]);
    assert(find_earliest_conn(heap) == conns[1]);
    for (int i = 0; i < 10; i++) {
        remove_from_heap(heap, conns[i]);
        destroy_connection(conns[i]);
    }
    assert(find_earliest_conn(heap) == NULL);
    destroy_timer_heap(heap);
}

int main (void) {
    test_order_and_update();
    test_remove_from_middle();
    printf("test_timer_heap: 모두 통과\n");
    return 0;
}
