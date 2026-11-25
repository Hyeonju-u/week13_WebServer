#include "buffer.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "settings.h"

/* 모든 버퍼의 합계를 한 곳에서 세어야 서버 전체 상한(NET-12)을 지킬 수 있다 */
static size_t total_bytes = 0;
static size_t total_limit = BUFFER_TOTAL_MAX;

static size_t compute_capacity (size_t current, size_t needed);

/* ── 공개 함수 ── */

void init_buffer (byte_buffer_t * buffer) {
    buffer->data = NULL;
    buffer->length = 0;
    buffer->capacity = 0;
}

/* 0이면 extra_length만큼 더 쓸 자리가 있다. -1이면 메모리 부족 또는 상한 초과 */
int reserve_buffer (byte_buffer_t * buffer, size_t extra_length) {
    size_t needed = buffer->length + extra_length;
    if (needed < buffer->length) {
        return -1;
    }
    if (needed <= buffer->capacity) {
        return 0;
    }
    size_t capacity = compute_capacity(buffer->capacity, needed);
    size_t growth = capacity - buffer->capacity;
    if (growth > total_limit - total_bytes) {
        return -1;
    }
    char * grown = realloc(buffer->data, capacity);
    if (grown == NULL) {
        return -1;
    }
    total_bytes += growth;
    buffer->data = grown;
    buffer->capacity = capacity;
    return 0;
}

int append_bytes (byte_buffer_t * buffer, const char * bytes, size_t length) {
    if (reserve_buffer(buffer, length) != 0) {
        return -1;
    }
    if (length > 0) {
        memcpy(buffer->data + buffer->length, bytes, length);
    }
    buffer->length += length;
    return 0;
}

/* 먼저 길이를 재고 그만큼 확보하므로 잘린 문자열이 들어갈 수 없다 (RES-07) */
int append_format (byte_buffer_t * buffer, const char * format, ...) {
    va_list arguments;
    va_start(arguments, format);
    int needed = vsnprintf(NULL, 0, format, arguments);
    va_end(arguments);
    if (needed < 0) {
        return -1;
    }
    if (reserve_buffer(buffer, (size_t) needed + 1) != 0) {
        return -1;
    }
    va_start(arguments, format);
    int written = vsnprintf(buffer->data + buffer->length,
        (size_t) needed + 1, format, arguments);
    va_end(arguments);
    if (written != needed) {
        return -1;
    }
    buffer->length += (size_t) needed;
    return 0;
}

/* 다 비워지면 늘어난 메모리를 돌려줘서 큰 요청 뒤에도 연결이 가볍게 남는다 */
void consume_bytes (byte_buffer_t * buffer, size_t length) {
    if (length >= buffer->length) {
        buffer->length = 0;
    } else {
        memmove(buffer->data, buffer->data + length, buffer->length - length);
        buffer->length -= length;
    }
    if (buffer->length == 0 && buffer->capacity > BUFFER_INITIAL_BYTES) {
        release_buffer(buffer);
    }
}

void release_buffer (byte_buffer_t * buffer) {
    total_bytes -= buffer->capacity;
    free(buffer->data);
    init_buffer(buffer);
}

void set_buffer_limit (size_t limit) {
    total_limit = limit;
}

size_t get_total_buffer_bytes (void) {
    return total_bytes;
}

/* ── 도우미 함수 ── */

/*
 * 보통은 두 배씩 키워서 복사 횟수를 줄인다. 하지만 64KB 파일 조각처럼 한 번에
 * 크게 필요하면 두 배로 올리다가 128KB를 잡게 되므로 필요한 만큼만 잡는다.
 */
static size_t compute_capacity (size_t current, size_t needed) {
    size_t capacity = current > 0 ? current : BUFFER_INITIAL_BYTES;
    if (needed / 2 > capacity && needed < SIZE_MAX / 2) {
        return (needed + BUFFER_INITIAL_BYTES - 1) / BUFFER_INITIAL_BYTES
            * BUFFER_INITIAL_BYTES;
    }
    while (capacity < needed) {
        if (capacity > SIZE_MAX / 2) {
            return needed;
        }
        capacity *= 2;
    }
    return capacity;
}
