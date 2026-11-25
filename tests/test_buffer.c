#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "buffer.h"
#include "settings.h"

static void test_append_and_consume (void) {
    byte_buffer_t buffer;
    init_buffer(&buffer);
    assert(append_bytes(&buffer, "hello", 5) == 0);
    assert(buffer.capacity == BUFFER_INITIAL_BYTES);
    assert(append_format(&buffer, " %s %d", "world", 42) == 0);
    assert(buffer.length == 14);
    assert(memcmp(buffer.data, "hello world 42", 14) == 0);
    consume_bytes(&buffer, 6);
    assert(buffer.length == 8);
    assert(memcmp(buffer.data, "world 42", 8) == 0);
    release_buffer(&buffer);
    assert(get_total_buffer_bytes() == 0);
}

/* 큰 요청이 끝나면 늘어난 메모리를 돌려줘야 한다 (NFR-06) */
static void test_shrink_when_empty (void) {
    byte_buffer_t buffer;
    init_buffer(&buffer);
    char chunk[10000];
    memset(chunk, 'x', sizeof(chunk));
    assert(append_bytes(&buffer, chunk, sizeof(chunk)) == 0);
    assert(buffer.capacity > BUFFER_INITIAL_BYTES);
    assert(get_total_buffer_bytes() == buffer.capacity);
    consume_bytes(&buffer, sizeof(chunk));
    assert(buffer.data == NULL && buffer.capacity == 0);
    assert(get_total_buffer_bytes() == 0);
}

static void test_total_limit (void) {
    byte_buffer_t first;
    byte_buffer_t second;
    init_buffer(&first);
    init_buffer(&second);
    set_buffer_limit(10000);
    assert(reserve_buffer(&first, 5000) == 0);
    assert(reserve_buffer(&second, 5000) != 0);
    assert(get_total_buffer_bytes() == first.capacity);
    release_buffer(&first);
    assert(reserve_buffer(&second, 5000) == 0);
    release_buffer(&second);
    set_buffer_limit(BUFFER_TOTAL_MAX);
    assert(get_total_buffer_bytes() == 0);
}

static void test_format_longer_than_stack (void) {
    byte_buffer_t buffer;
    init_buffer(&buffer);
    char long_text[3000];
    memset(long_text, 'a', sizeof(long_text) - 1);
    long_text[sizeof(long_text) - 1] = '\0';
    assert(append_format(&buffer, "[%s]", long_text) == 0);
    assert(buffer.length == sizeof(long_text) + 1);
    assert(buffer.data[buffer.length - 1] == ']');
    release_buffer(&buffer);
}

int main (void) {
    test_append_and_consume();
    test_shrink_when_empty();
    test_total_limit();
    test_format_longer_than_stack();
    printf("test_buffer: 모두 통과\n");
    return 0;
}
