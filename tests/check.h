#ifndef CHECK_H
#define CHECK_H

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

/* (B6, P35) 실패해도 멈추지 않고 끝에 개수만 센다.
 * 카운터가 static이라 테스트 파일 하나에서만 포함한다 */
static int check_passed_count = 0;
static int check_failed_count = 0;
static const char * check_current_id = "";

#define BEGIN_TEST(id) (check_current_id = (id))

#define CHECK(condition) \
    record_check((condition), #condition, __FILE__, __LINE__)

/* 실제 값은 한 번만 계산한다 */
#define CHECK_INT(actual, expected) \
    record_int_check((long long) (actual), (long long) (expected), \
                     #actual, __FILE__, __LINE__)

#define CHECK_VIEW(pointer, length, expected) \
    record_view_check((pointer), (length), (expected), #pointer, \
                      __FILE__, __LINE__)

static void record_check (bool passed, const char * expression,
                          const char * file, int line) {
    if (passed) {
        check_passed_count++;
        return;
    }
    check_failed_count++;
    printf("[%s] %s:%d: CHECK(%s) 실패\n",
           check_current_id, file, line, expression);
}

static void record_int_check (long long actual, long long expected,
                              const char * expression,
                              const char * file, int line) {
    if (actual == expected) {
        check_passed_count++;
        return;
    }
    check_failed_count++;
    printf("[%s] %s:%d: %s 실제 %lld, 기대 %lld\n",
           check_current_id, file, line, expression, actual, expected);
}

static void record_view_check (const char * pointer, size_t length,
                               const char * expected,
                               const char * expression,
                               const char * file, int line) {
    if (pointer != NULL && length == strlen(expected)
        && memcmp(pointer, expected, length) == 0) {
        check_passed_count++;
        return;
    }
    check_failed_count++;
    if (pointer == NULL) {
        printf("[%s] %s:%d: %s 실제 NULL, 기대 \"%s\"\n",
               check_current_id, file, line, expression, expected);
        return;
    }
    printf("[%s] %s:%d: %s 실제 \"%.*s\", 기대 \"%s\"\n",
           check_current_id, file, line, expression,
           (int) length, pointer, expected);
}

/* 반환: 실패가 하나라도 있으면 1 (main의 종료 코드로 쓴다) */
static int report_checks (void) {
    printf("통과 %d / 실패 %d\n", check_passed_count, check_failed_count);
    return check_failed_count > 0 ? 1 : 0;
}

#endif
