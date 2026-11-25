#include "timer_heap.h"

#include <stdlib.h>
#include <time.h>

#define HEAP_INITIAL_CAPACITY 1024

struct timer_heap {
    conn_t ** items;
    size_t count;
    size_t capacity;
};

static int grow_heap (timer_heap_t * heap);
static void sift_up (timer_heap_t * heap, size_t index);
static void swap_items (timer_heap_t * heap, size_t first, size_t second);
static void sift_down (timer_heap_t * heap, size_t index);

/* ── 공개 함수 ── */

timer_heap_t * create_timer_heap (void) {
    timer_heap_t * heap = calloc(1, sizeof(*heap));
    return heap;
}

void destroy_timer_heap (timer_heap_t * heap) {
    free(heap->items);
    free(heap);
}

/* 이미 들어 있으면 마감 시각만 바꾸고 자리를 다시 잡는다 */
int set_heap_deadline (timer_heap_t * heap, conn_t * conn,
    long long deadline_ms) {
    if (conn->heap_index < 0) {
        if (grow_heap(heap) != 0) {
            return -1;
        }
        conn->deadline_ms = deadline_ms;
        conn->heap_index = (long) heap->count;
        heap->items[heap->count++] = conn;
    } else {
        conn->deadline_ms = deadline_ms;
    }
    sift_up(heap, (size_t) conn->heap_index);
    sift_down(heap, (size_t) conn->heap_index);
    return 0;
}

void remove_from_heap (timer_heap_t * heap, conn_t * conn) {
    if (conn->heap_index < 0) {
        return;
    }
    size_t index = (size_t) conn->heap_index;
    size_t last = heap->count - 1;
    if (index != last) {
        swap_items(heap, index, last);
    }
    conn->heap_index = -1;
    heap->count--;
    if (index < heap->count) {
        sift_up(heap, index);
        sift_down(heap, index);
    }
}

conn_t * find_earliest_conn (const timer_heap_t * heap) {
    return heap->count == 0 ? NULL : heap->items[0];
}

/* 시계가 거꾸로 가면 마감이 틀어지므로 단조 시계를 쓴다 */
long long get_now_ms (void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (long long) now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

/* ── 도우미 함수 ── */

static int grow_heap (timer_heap_t * heap) {
    if (heap->count < heap->capacity) {
        return 0;
    }
    size_t capacity = heap->capacity == 0 ? HEAP_INITIAL_CAPACITY
        : heap->capacity * 2;
    conn_t ** grown = realloc(heap->items, capacity * sizeof(*grown));
    if (grown == NULL) {
        return -1;
    }
    heap->items = grown;
    heap->capacity = capacity;
    return 0;
}

static void sift_up (timer_heap_t * heap, size_t index) {
    while (index > 0) {
        size_t parent = (index - 1) / 2;
        conn_t * parent_conn = heap->items[parent];
        if (parent_conn->deadline_ms <= heap->items[index]->deadline_ms) {
            return;
        }
        swap_items(heap, parent, index);
        index = parent;
    }
}

static void swap_items (timer_heap_t * heap, size_t first, size_t second) {
    conn_t * held = heap->items[first];
    heap->items[first] = heap->items[second];
    heap->items[second] = held;
    heap->items[first]->heap_index = (long) first;
    heap->items[second]->heap_index = (long) second;
}

static void sift_down (timer_heap_t * heap, size_t index) {
    for (;;) {
        size_t smallest = index;
        size_t left = index * 2 + 1;
        size_t right = index * 2 + 2;
        if (left < heap->count
            && heap->items[left]->deadline_ms
                < heap->items[smallest]->deadline_ms) {
            smallest = left;
        }
        if (right < heap->count
            && heap->items[right]->deadline_ms
                < heap->items[smallest]->deadline_ms) {
            smallest = right;
        }
        if (smallest == index) {
            return;
        }
        swap_items(heap, index, smallest);
        index = smallest;
    }
}
