#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "http_request.h"
#include "check.h"

/* 8KB 경계 테스트(F5, F6, F10, P-10)가 8,193바이트까지 쓴다 */
#define LARGE_BUFFER_SIZE (MAX_HEADER_SIZE + 64)

static void test_f1_header_with_empty_line (void);
static void test_f2_no_empty_line_yet (void);
static void test_f3_two_requests_back_to_back (void);
static void test_f4_one_byte_at_a_time (void);
static void test_f5_request_line_over_limit (void);
static void fill_long_request_line (char * buffer, size_t length);
static void test_f6_header_over_limit (void);
static void fill_header_without_end (char * buffer, size_t length);
static void test_f7_newline_without_carriage_return (void);
static void test_f8_leading_empty_lines (void);
static void load_buffer (char * buffer, size_t * length, const char * text);
static void test_f9_leading_bare_newline (void);
static void test_f10_header_size_boundary (void);
static void fill_header_of_length (char * buffer, size_t length);
static void test_f11_lone_carriage_return (void);
static void test_f12_buffer_ends_with_carriage_return (void);
static void test_h1_three_lines (void);
static void test_h2_value_trimmed (void);
static void test_h3_space_before_colon (void);
static void test_h4_line_starts_with_space (void);
static void test_h5_line_without_colon (void);
static void test_p1_get_with_host (void);
static parse_result parse_text (const char * text, request_t * request);
static parse_result parse_bytes (const char * bytes, size_t length,
                                 request_t * request);
static void test_p2_post_content_length (void);
static void test_p3_lowercase_header_name (void);
static void test_p4_connection_close_or_absent (void);
static void test_p5_connection_list_with_close (void);
static void test_p6_absolute_form_path (void);
static void test_p7_absolute_form_host_wins (void);
static void test_p8_path_kept_raw (void);
static void test_p9_get_with_content_length (void);
static void test_p10_many_short_headers (void);
static size_t fill_many_headers (char * buffer, size_t count);
static void test_p11_body_size_boundary (void);
static void test_p12_headers_walk_after_parse (void);
static void test_p13_request_line_one_word (void);
static void check_bad_request (const char * text, int expected_status);
static void test_p14_double_space (void);
static void test_p15_four_parts (void);
static void test_p16_path_not_slash_or_absolute (void);
static void test_p17_unsupported_methods (void);
static void test_p18_lowercase_method (void);
static void test_p19_unsupported_versions (void);
static void test_p20_bad_header_lines (void);
static void test_p21_host_missing_or_twice (void);
static void test_p22_post_without_content_length (void);
static void test_p23_content_length_not_digits (void);
static void test_p24_content_length_overflow (void);
static void test_p25_content_length_over_limit (void);
static void test_p26_content_length_twice (void);
static void test_p27_transfer_encoding (void);
static void test_p28_version_checked_before_length (void);
static void test_p29_method_checked_before_host (void);
static void test_p30_transfer_encoding_without_length (void);
static void test_p31_empty_host (void);
static void test_p32_connection_lines_twice (void);
static void test_p33_content_length_digits_rule (void);
static void test_b1_whole_body_at_once (void);
static bool prepare_body (const char * text, char * buffer, size_t * length,
                          request_t * request);
static void test_b2_partial_body (void);
static void test_b3_body_followed_by_next_request (void);
static void test_b4_no_body_buffer (void);
static void test_b5_get_body_discarded (void);
static void test_b6_rest_of_body_written_directly (void);
static void test_l1_head_copy_survives_overwrite (void);
static void test_l2_destroy_twice (void);
static void test_l3_all_paths_release_memory (void);

/* ── 공개 함수 ── */

int main (void) {
    test_f1_header_with_empty_line();
    test_f2_no_empty_line_yet();
    test_f3_two_requests_back_to_back();
    test_f4_one_byte_at_a_time();
    test_f5_request_line_over_limit();
    test_f6_header_over_limit();
    test_f7_newline_without_carriage_return();
    test_f8_leading_empty_lines();
    test_f9_leading_bare_newline();
    test_f10_header_size_boundary();
    test_f11_lone_carriage_return();
    test_f12_buffer_ends_with_carriage_return();

    test_h1_three_lines();
    test_h2_value_trimmed();
    test_h3_space_before_colon();
    test_h4_line_starts_with_space();
    test_h5_line_without_colon();

    test_p1_get_with_host();
    test_p2_post_content_length();
    test_p3_lowercase_header_name();
    test_p4_connection_close_or_absent();
    test_p5_connection_list_with_close();
    test_p6_absolute_form_path();
    test_p7_absolute_form_host_wins();
    test_p8_path_kept_raw();
    test_p9_get_with_content_length();
    test_p10_many_short_headers();
    test_p11_body_size_boundary();
    test_p12_headers_walk_after_parse();
    test_p13_request_line_one_word();
    test_p14_double_space();
    test_p15_four_parts();
    test_p16_path_not_slash_or_absolute();
    test_p17_unsupported_methods();
    test_p18_lowercase_method();
    test_p19_unsupported_versions();
    test_p20_bad_header_lines();
    test_p21_host_missing_or_twice();
    test_p22_post_without_content_length();
    test_p23_content_length_not_digits();
    test_p24_content_length_overflow();
    test_p25_content_length_over_limit();
    test_p26_content_length_twice();
    test_p27_transfer_encoding();
    test_p28_version_checked_before_length();
    test_p29_method_checked_before_host();
    test_p30_transfer_encoding_without_length();
    test_p31_empty_host();
    test_p32_connection_lines_twice();
    test_p33_content_length_digits_rule();

    test_b1_whole_body_at_once();
    test_b2_partial_body();
    test_b3_body_followed_by_next_request();
    test_b4_no_body_buffer();
    test_b5_get_body_discarded();
    test_b6_rest_of_body_written_directly();

    test_l1_head_copy_survives_overwrite();
    test_l2_destroy_twice();
    test_l3_all_paths_release_memory();

    return report_checks();
}

/* ── F. find_header_end ── */

/* F1 (R1c) */
static void test_f1_header_with_empty_line (void) {
    const char * text = "GET / HTTP/1.1\r\nHost: x\r\n\r\n";
    size_t header_length = 0;
    int status = 0;

    BEGIN_TEST("F1");
    CHECK_INT(find_header_end(text, strlen(text), &header_length, &status),
              FIND_FOUND);
    CHECK_INT(header_length, strlen(text));
}

/* F2 (R1c) */
static void test_f2_no_empty_line_yet (void) {
    const char * text = "GET / HTTP/1.1\r\nHost: x\r\n";
    size_t header_length = 0;
    int status = 0;

    BEGIN_TEST("F2");
    CHECK_INT(find_header_end(text, strlen(text), &header_length, &status),
              FIND_NEED_MORE);
}

/* F3 (R3d) */
static void test_f3_two_requests_back_to_back (void) {
    const char * first = "GET /a HTTP/1.1\r\nHost: x\r\n\r\n";
    const char * text = "GET /a HTTP/1.1\r\nHost: x\r\n\r\n"
                        "GET /b HTTP/1.1\r\nHost: x\r\n\r\n";
    size_t header_length = 0;
    int status = 0;

    BEGIN_TEST("F3");
    CHECK_INT(find_header_end(text, strlen(text), &header_length, &status),
              FIND_FOUND);
    CHECK_INT(header_length, strlen(first));
}

/* F4 (R3c) 마지막 바이트 전까지는 전부 NEED_MORE여야 한다 */
static void test_f4_one_byte_at_a_time (void) {
    const char * text = "GET / HTTP/1.1\r\nHost: x\r\n\r\n";
    size_t full_length = strlen(text);
    size_t header_length = 0;
    size_t need_more_count = 0;
    int status = 0;

    BEGIN_TEST("F4");
    for (size_t length = 1; length < full_length; length++) {
        if (find_header_end(text, length, &header_length, &status)
            == FIND_NEED_MORE) {
            need_more_count++;
        }
    }
    CHECK_INT(need_more_count, full_length - 1);
    CHECK_INT(find_header_end(text, full_length, &header_length, &status),
              FIND_FOUND);
}

/* F5 (B4) */
static void test_f5_request_line_over_limit (void) {
    static char buffer[LARGE_BUFFER_SIZE];
    size_t header_length = 0;
    int status = 0;

    BEGIN_TEST("F5");
    fill_long_request_line(buffer, MAX_HEADER_SIZE);
    CHECK_INT(find_header_end(buffer, MAX_HEADER_SIZE, &header_length,
                              &status),
              FIND_BAD);
    CHECK_INT(status, 414);
}

/* \r\n이 하나도 없는 "GET /aaa..."를 length바이트 채운다 */
static void fill_long_request_line (char * buffer, size_t length) {
    const char * prefix = "GET /";
    size_t prefix_length = strlen(prefix);

    memcpy(buffer, prefix, prefix_length);
    memset(buffer + prefix_length, 'a', length - prefix_length);
}

/* F6 (B4) */
static void test_f6_header_over_limit (void) {
    static char buffer[LARGE_BUFFER_SIZE];
    size_t header_length = 0;
    int status = 0;

    BEGIN_TEST("F6");
    fill_header_without_end(buffer, MAX_HEADER_SIZE);
    CHECK_INT(find_header_end(buffer, MAX_HEADER_SIZE, &header_length,
                              &status),
              FIND_BAD);
    CHECK_INT(status, 431);
}

/* 첫 줄은 끝나지만 빈 줄은 없는 헤더를 정확히 length바이트 채운다 */
static void fill_header_without_end (char * buffer, size_t length) {
    const char * prefix = "GET / HTTP/1.1\r\nX: ";
    size_t prefix_length = strlen(prefix);
    size_t fill_length = length - prefix_length - 2;

    memcpy(buffer, prefix, prefix_length);
    memset(buffer + prefix_length, 'a', fill_length);
    memcpy(buffer + prefix_length + fill_length, "\r\n", 2);
}

/* F7 (B29) */
static void test_f7_newline_without_carriage_return (void) {
    const char * text = "GET / HTTP/1.1\r\nHost: x\nAccept: y\r\n\r\n";
    size_t header_length = 0;
    int status = 0;

    BEGIN_TEST("F7");
    CHECK_INT(find_header_end(text, strlen(text), &header_length, &status),
              FIND_BAD);
    CHECK_INT(status, 400);
}

/* F8 (P3, P17) */
static void test_f8_leading_empty_lines (void) {
    const char * request = "GET / HTTP/1.1\r\nHost: x\r\n\r\n";
    char buffer[MAX_HEADER_SIZE];
    size_t length = 0;
    size_t header_length = 0;
    int status = 0;

    BEGIN_TEST("F8");
    load_buffer(buffer, &length, "\r\n\r\nGET / HTTP/1.1\r\nHost: x\r\n\r\n");
    CHECK(skip_leading_empty_lines(buffer, &length));
    CHECK_INT(find_header_end(buffer, length, &header_length, &status),
              FIND_FOUND);
    CHECK_INT(header_length, strlen(request));
}

static void load_buffer (char * buffer, size_t * length, const char * text) {
    *length = strlen(text);
    memcpy(buffer, text, *length);
}

/* F9 (B29, P3) skip_leading_empty_lines의 false = 400 (P17) */
static void test_f9_leading_bare_newline (void) {
    char buffer[MAX_HEADER_SIZE];
    size_t length = 0;

    BEGIN_TEST("F9");
    load_buffer(buffer, &length, "\nGET / HTTP/1.1\r\nHost: x\r\n\r\n");
    CHECK(!skip_leading_empty_lines(buffer, &length));
}

/* F10 (B4 경계) */
static void test_f10_header_size_boundary (void) {
    static char buffer[LARGE_BUFFER_SIZE];
    size_t header_length = 0;
    int status = 0;

    BEGIN_TEST("F10");
    fill_header_of_length(buffer, MAX_HEADER_SIZE);
    CHECK_INT(find_header_end(buffer, MAX_HEADER_SIZE, &header_length,
                              &status),
              FIND_FOUND);
    CHECK_INT(header_length, MAX_HEADER_SIZE);

    fill_header_of_length(buffer, MAX_HEADER_SIZE + 1);
    CHECK_INT(find_header_end(buffer, MAX_HEADER_SIZE + 1, &header_length,
                              &status),
              FIND_BAD);
    CHECK_INT(status, 431);
}

/* 마지막 빈 줄까지 포함해 정확히 length바이트인 헤더를 만든다 (P29) */
static void fill_header_of_length (char * buffer, size_t length) {
    const char * prefix = "GET / HTTP/1.1\r\nHost: x\r\nX: ";
    const char * suffix = "\r\n\r\n";
    size_t prefix_length = strlen(prefix);
    size_t suffix_length = strlen(suffix);
    size_t fill_length = length - prefix_length - suffix_length;

    memcpy(buffer, prefix, prefix_length);
    memset(buffer + prefix_length, 'a', fill_length);
    memcpy(buffer + prefix_length + fill_length, suffix, suffix_length);
}

/* F11 (P30) */
static void test_f11_lone_carriage_return (void) {
    const char * text = "GET / HTTP/1.1\r\nHost: a\rb\r\n\r\n";
    size_t header_length = 0;
    int status = 0;

    BEGIN_TEST("F11");
    CHECK_INT(find_header_end(text, strlen(text), &header_length, &status),
              FIND_BAD);
    CHECK_INT(status, 400);
}

/* F12 (P29) 다음 바이트가 \n일 수 있다 */
static void test_f12_buffer_ends_with_carriage_return (void) {
    const char * text = "GET / HTTP/1.1\r\nHost: x\r";
    size_t header_length = 0;
    int status = 0;

    BEGIN_TEST("F12");
    CHECK_INT(find_header_end(text, strlen(text), &header_length, &status),
              FIND_NEED_MORE);
}

/* ── H. next_header_line ── */

/* H1 (P13) block은 헤더 줄 영역만 (P22) */
static void test_h1_three_lines (void) {
    const char * block = "Host: x\r\nAccept: y\r\nUser-Agent: z\r\n";
    size_t position = 0;
    header_view_t line;

    BEGIN_TEST("H1");
    CHECK_INT(next_header_line(block, strlen(block), &position, &line),
              LINE_OK);
    CHECK_INT(next_header_line(block, strlen(block), &position, &line),
              LINE_OK);
    CHECK_INT(next_header_line(block, strlen(block), &position, &line),
              LINE_OK);
    CHECK_INT(next_header_line(block, strlen(block), &position, &line),
              LINE_END);
}

/* H2 (P6) */
static void test_h2_value_trimmed (void) {
    const char * block = "Content-Length:   16  \r\n";
    size_t position = 0;
    header_view_t line;

    BEGIN_TEST("H2");
    CHECK_INT(next_header_line(block, strlen(block), &position, &line),
              LINE_OK);
    CHECK_VIEW(line.value, line.value_length, "16");
}

/* H3 (P5) */
static void test_h3_space_before_colon (void) {
    const char * block = "Host : x\r\n";
    size_t position = 0;
    header_view_t line;

    BEGIN_TEST("H3");
    CHECK_INT(next_header_line(block, strlen(block), &position, &line),
              LINE_BAD);
}

/* H4 (P5) 줄 접기 */
static void test_h4_line_starts_with_space (void) {
    const char * block = " Host: x\r\n";
    size_t position = 0;
    header_view_t line;

    BEGIN_TEST("H4");
    CHECK_INT(next_header_line(block, strlen(block), &position, &line),
              LINE_BAD);
}

/* H5 (P5) */
static void test_h5_line_without_colon (void) {
    const char * block = "Host x\r\n";
    size_t position = 0;
    header_view_t line;

    BEGIN_TEST("H5");
    CHECK_INT(next_header_line(block, strlen(block), &position, &line),
              LINE_BAD);
}

/* ── P. parse_request_head ── */

/* P-1 (R1b) */
static void test_p1_get_with_host (void) {
    request_t request = {0};

    BEGIN_TEST("P-1");
    CHECK_INT(parse_text("GET /index.html HTTP/1.1\r\nHost: x\r\n\r\n",
                         &request),
              PARSE_OK);
    CHECK_INT(request.method, METHOD_GET);
    CHECK_VIEW(request.path, request.path_length, "/index.html");
    CHECK_VIEW(request.version, request.version_length, "HTTP/1.1");
    destroy_request(&request);
}

/* 실제 흐름처럼 복사본을 만든 뒤 복사본에 대고 파싱한다 (P14) */
static parse_result parse_text (const char * text, request_t * request) {
    return parse_bytes(text, strlen(text), request);
}

/* 반환: 복사에 실패하면 PARSE_BAD, status 500 */
static parse_result parse_bytes (const char * bytes, size_t length,
                                 request_t * request) {
    if (!copy_request_head(request, bytes, length)) {
        request->status = 500;
        return PARSE_BAD;
    }
    return parse_request_head(request->head, request->head_length, request);
}

/* P-2 (R2a) */
static void test_p2_post_content_length (void) {
    request_t request = {0};

    BEGIN_TEST("P-2");
    CHECK_INT(parse_text("POST /a HTTP/1.1\r\nHost: x\r\n"
                         "Content-Length: 16\r\n\r\n",
                         &request),
              PARSE_OK);
    CHECK_INT(request.content_length, 16);
    destroy_request(&request);
}

/* P-3 (R1c) */
static void test_p3_lowercase_header_name (void) {
    request_t request = {0};

    BEGIN_TEST("P-3");
    CHECK_INT(parse_text("POST /a HTTP/1.1\r\nHost: x\r\n"
                         "content-length: 16\r\n\r\n",
                         &request),
              PARSE_OK);
    CHECK_INT(request.content_length, 16);
    destroy_request(&request);
}

/* P-4 (P8) */
static void test_p4_connection_close_or_absent (void) {
    request_t closing = {0};
    request_t keeping = {0};

    BEGIN_TEST("P-4");
    CHECK_INT(parse_text("GET /a HTTP/1.1\r\nHost: x\r\n"
                         "Connection: close\r\n\r\n",
                         &closing),
              PARSE_OK);
    CHECK(!closing.keep_alive);
    CHECK_INT(parse_text("GET /a HTTP/1.1\r\nHost: x\r\n\r\n", &keeping),
              PARSE_OK);
    CHECK(keeping.keep_alive);
    destroy_request(&closing);
    destroy_request(&keeping);
}

/* P-5 (P8) */
static void test_p5_connection_list_with_close (void) {
    request_t request = {0};

    BEGIN_TEST("P-5");
    CHECK_INT(parse_text("GET /a HTTP/1.1\r\nHost: x\r\n"
                         "Connection: Keep-Alive, CLOSE\r\n\r\n",
                         &request),
              PARSE_OK);
    CHECK(!request.keep_alive);
    destroy_request(&request);
}

/* P-6 (P4) Host 헤더는 절대 주소 형식이어도 필요하다 (B2) */
static void test_p6_absolute_form_path (void) {
    request_t request = {0};

    BEGIN_TEST("P-6");
    CHECK_INT(parse_text("GET http://localhost:8080/a HTTP/1.1\r\n"
                         "Host: localhost:8080\r\n\r\n",
                         &request),
              PARSE_OK);
    CHECK_VIEW(request.path, request.path_length, "/a");
    destroy_request(&request);
}

/* P-7 (P4, P23) */
static void test_p7_absolute_form_host_wins (void) {
    request_t request = {0};

    BEGIN_TEST("P-7");
    CHECK_INT(parse_text("GET http://localhost:8080/a HTTP/1.1\r\n"
                         "Host: other.example\r\n\r\n",
                         &request),
              PARSE_OK);
    CHECK_VIEW(request.host, request.host_length, "localhost:8080");
    destroy_request(&request);
}

/* P-8 (P12) */
static void test_p8_path_kept_raw (void) {
    request_t request = {0};

    BEGIN_TEST("P-8");
    CHECK_INT(parse_text("GET /a%20b.html?x=1 HTTP/1.1\r\nHost: x\r\n\r\n",
                         &request),
              PARSE_OK);
    CHECK_VIEW(request.path, request.path_length, "/a%20b.html?x=1");
    destroy_request(&request);
}

/* P-9 (P10) */
static void test_p9_get_with_content_length (void) {
    request_t request = {0};

    BEGIN_TEST("P-9");
    CHECK_INT(parse_text("GET /a HTTP/1.1\r\nHost: x\r\n"
                         "Content-Length: 5\r\n\r\n",
                         &request),
              PARSE_OK);
    CHECK_INT(request.content_length, 5);
    destroy_request(&request);
}

/* P-10 (P7) */
static void test_p10_many_short_headers (void) {
    static char buffer[LARGE_BUFFER_SIZE];
    request_t request = {0};
    size_t length = fill_many_headers(buffer, 500);

    BEGIN_TEST("P-10");
    CHECK(length <= MAX_HEADER_SIZE);
    CHECK_INT(parse_bytes(buffer, length, &request), PARSE_OK);
    destroy_request(&request);
}

/* 반환: 만든 헤더의 바이트 수 (마지막 빈 줄 포함) */
static size_t fill_many_headers (char * buffer, size_t count) {
    const char * first = "GET / HTTP/1.1\r\nHost: x\r\n";
    const char * line = "A: b\r\n";
    size_t length = 0;

    load_buffer(buffer, &length, first);
    for (size_t i = 0; i < count; i++) {
        memcpy(buffer + length, line, strlen(line));
        length += strlen(line);
    }
    memcpy(buffer + length, "\r\n", 2);
    return length + 2;
}

/* P-11 (B4 경계) */
static void test_p11_body_size_boundary (void) {
    request_t request = {0};

    BEGIN_TEST("P-11");
    CHECK_INT(parse_text("POST /a HTTP/1.1\r\nHost: x\r\n"
                         "Content-Length: 1048576\r\n\r\n",
                         &request),
              PARSE_OK);
    CHECK_INT(request.content_length, 1048576);
    destroy_request(&request);
}

/* P-12 (P13) */
static void test_p12_headers_walk_after_parse (void) {
    request_t request = {0};
    size_t position = 0;
    header_view_t line;

    BEGIN_TEST("P-12");
    CHECK_INT(parse_text("POST /a HTTP/1.1\r\nHost: x\r\n"
                         "Content-Type: text/plain\r\n"
                         "Content-Length: 4\r\n\r\n",
                         &request),
              PARSE_OK);
    CHECK_INT(next_header_line(request.headers, request.headers_length,
                               &position, &line),
              LINE_OK);
    CHECK_VIEW(line.name, line.name_length, "Host");
    CHECK_INT(next_header_line(request.headers, request.headers_length,
                               &position, &line),
              LINE_OK);
    CHECK_VIEW(line.name, line.name_length, "Content-Type");
    CHECK_INT(next_header_line(request.headers, request.headers_length,
                               &position, &line),
              LINE_OK);
    CHECK_VIEW(line.name, line.name_length, "Content-Length");
    CHECK_INT(next_header_line(request.headers, request.headers_length,
                               &position, &line),
              LINE_END);
    destroy_request(&request);
}

/* P-13 (P2) */
static void test_p13_request_line_one_word (void) {
    BEGIN_TEST("P-13");
    check_bad_request("HELLO\r\nHost: x\r\n\r\n", 400);
}

static void check_bad_request (const char * text, int expected_status) {
    request_t request = {0};

    CHECK_INT(parse_text(text, &request), PARSE_BAD);
    CHECK_INT(request.status, expected_status);
    destroy_request(&request);
}

/* P-14 (P2) */
static void test_p14_double_space (void) {
    BEGIN_TEST("P-14");
    check_bad_request("GET  /a HTTP/1.1\r\nHost: x\r\n\r\n", 400);
}

/* P-15 (P2) */
static void test_p15_four_parts (void) {
    BEGIN_TEST("P-15");
    check_bad_request("GET /a b HTTP/1.1\r\nHost: x\r\n\r\n", 400);
}

/* P-16 (P4) */
static void test_p16_path_not_slash_or_absolute (void) {
    BEGIN_TEST("P-16");
    check_bad_request("GET a.html HTTP/1.1\r\nHost: x\r\n\r\n", 400);
}

/* P-17 (B5) */
static void test_p17_unsupported_methods (void) {
    BEGIN_TEST("P-17");
    check_bad_request("PUT /a HTTP/1.1\r\nHost: x\r\n\r\n", 501);
    check_bad_request("HEAD /a HTTP/1.1\r\nHost: x\r\n\r\n", 501);
}

/* P-18 (P1) */
static void test_p18_lowercase_method (void) {
    BEGIN_TEST("P-18");
    check_bad_request("get /a HTTP/1.1\r\nHost: x\r\n\r\n", 501);
}

/* P-19 (B1) */
static void test_p19_unsupported_versions (void) {
    BEGIN_TEST("P-19");
    check_bad_request("GET /a HTTP/1.0\r\nHost: x\r\n\r\n", 505);
    check_bad_request("GET /a HTTP/9.9\r\nHost: x\r\n\r\n", 505);
}

/* P-20 (P5) */
static void test_p20_bad_header_lines (void) {
    BEGIN_TEST("P-20");
    check_bad_request("GET /a HTTP/1.1\r\nHost: x\r\nAccept y\r\n\r\n", 400);
    check_bad_request("GET /a HTTP/1.1\r\nHost : x\r\n\r\n", 400);
}

/* P-21 (B2) */
static void test_p21_host_missing_or_twice (void) {
    BEGIN_TEST("P-21");
    check_bad_request("GET /a HTTP/1.1\r\n\r\n", 400);
    check_bad_request("GET /a HTTP/1.1\r\nHost: x\r\nHost: y\r\n\r\n", 400);
}

/* P-22 (B3) */
static void test_p22_post_without_content_length (void) {
    BEGIN_TEST("P-22");
    check_bad_request("POST /a HTTP/1.1\r\nHost: x\r\n\r\n", 411);
}

/* P-23 (B3, P9) */
static void test_p23_content_length_not_digits (void) {
    BEGIN_TEST("P-23");
    check_bad_request("POST /a HTTP/1.1\r\nHost: x\r\n"
                      "Content-Length: abc\r\n\r\n", 400);
    check_bad_request("POST /a HTTP/1.1\r\nHost: x\r\n"
                      "Content-Length: -1\r\n\r\n", 400);
    check_bad_request("POST /a HTTP/1.1\r\nHost: x\r\n"
                      "Content-Length: +5\r\n\r\n", 400);
    check_bad_request("POST /a HTTP/1.1\r\nHost: x\r\n"
                      "Content-Length: \r\n\r\n", 400);
}

/* P-24 (P9) 넘쳐서 작은 값이 되지 않아야 한다 */
static void test_p24_content_length_overflow (void) {
    BEGIN_TEST("P-24");
    check_bad_request("POST /a HTTP/1.1\r\nHost: x\r\n"
                      "Content-Length: 99999999999999999999\r\n\r\n", 413);
}

/* P-25 (B4 경계) */
static void test_p25_content_length_over_limit (void) {
    BEGIN_TEST("P-25");
    check_bad_request("POST /a HTTP/1.1\r\nHost: x\r\n"
                      "Content-Length: 1048577\r\n\r\n", 413);
}

/* P-26 (B30) */
static void test_p26_content_length_twice (void) {
    BEGIN_TEST("P-26");
    check_bad_request("POST /a HTTP/1.1\r\nHost: x\r\n"
                      "Content-Length: 5\r\nContent-Length: 5\r\n\r\n", 400);
    check_bad_request("POST /a HTTP/1.1\r\nHost: x\r\n"
                      "Content-Length: 5\r\nContent-Length: 6\r\n\r\n", 400);
}

/* P-27 (B3) */
static void test_p27_transfer_encoding (void) {
    BEGIN_TEST("P-27");
    check_bad_request("GET /a HTTP/1.1\r\nHost: x\r\n"
                      "Transfer-Encoding: chunked\r\n\r\n", 501);
}

/* P-28 (B32) 버전이 먼저 */
static void test_p28_version_checked_before_length (void) {
    BEGIN_TEST("P-28");
    check_bad_request("POST /a HTTP/1.0\r\nHost: x\r\n"
                      "Content-Length: abc\r\n\r\n", 505);
}

/* P-29 (B32) 메서드가 먼저 */
static void test_p29_method_checked_before_host (void) {
    BEGIN_TEST("P-29");
    check_bad_request("PUT /a HTTP/1.1\r\n\r\n", 501);
}

/* P-30 (P18) */
static void test_p30_transfer_encoding_without_length (void) {
    BEGIN_TEST("P-30");
    check_bad_request("POST /a HTTP/1.1\r\nHost: x\r\n"
                      "Transfer-Encoding: chunked\r\n\r\n", 501);
}

/* P-31 (P32) */
static void test_p31_empty_host (void) {
    BEGIN_TEST("P-31");
    check_bad_request("GET /a HTTP/1.1\r\nHost:\r\n\r\n", 400);
}

/* P-32 (P33) */
static void test_p32_connection_lines_twice (void) {
    request_t request = {0};

    BEGIN_TEST("P-32");
    CHECK_INT(parse_text("GET /a HTTP/1.1\r\nHost: x\r\n"
                         "Connection: keep-alive\r\n"
                         "Connection: close\r\n\r\n",
                         &request),
              PARSE_OK);
    CHECK(!request.keep_alive);
    destroy_request(&request);
}

/* P-33 (P31) */
static void test_p33_content_length_digits_rule (void) {
    request_t request = {0};

    BEGIN_TEST("P-33");
    check_bad_request("POST /a HTTP/1.1\r\nHost: x\r\n"
                      "Content-Length: 99999abc\r\n\r\n", 400);
    CHECK_INT(parse_text("POST /a HTTP/1.1\r\nHost: x\r\n"
                         "Content-Length: 007\r\n\r\n",
                         &request),
              PARSE_OK);
    CHECK_INT(request.content_length, 7);
    destroy_request(&request);
}

/* ── B. 본문 모으기 ── */

/* B-1 (P11) */
static void test_b1_whole_body_at_once (void) {
    char buffer[MAX_HEADER_SIZE];
    size_t length = 0;
    request_t request = {0};

    BEGIN_TEST("B-1");
    CHECK(prepare_body("POST /a HTTP/1.1\r\nHost: x\r\n"
                       "Content-Length: 16\r\n\r\n"
                       "0123456789abcdef",
                       buffer, &length, &request));
    CHECK_INT(start_body(&request, buffer, &length), BODY_DONE);
    CHECK_VIEW(request.body, request.body_received, "0123456789abcdef");
    CHECK_INT(length, 0);
    destroy_request(&request);
}

/* 연결 버퍼 흐름(명세 2번)의 찾기 → 복사 → 파싱 → 헤더 지우기까지 밟는다.
 * 반환: false = 그 중 한 단계가 실패 */
static bool prepare_body (const char * text, char * buffer, size_t * length,
                          request_t * request) {
    size_t header_length = 0;
    int status = 0;

    load_buffer(buffer, length, text);
    if (find_header_end(buffer, *length, &header_length, &status)
        != FIND_FOUND) {
        return false;
    }
    if (!copy_request_head(request, buffer, header_length)) {
        return false;
    }
    if (parse_request_head(request->head, request->head_length, request)
        != PARSE_OK) {
        return false;
    }
    remove_buffer_prefix(buffer, length, header_length);
    return true;
}

/* B-2 (P11) */
static void test_b2_partial_body (void) {
    char buffer[MAX_HEADER_SIZE];
    size_t length = 0;
    request_t request = {0};

    BEGIN_TEST("B-2");
    CHECK(prepare_body("POST /a HTTP/1.1\r\nHost: x\r\n"
                       "Content-Length: 16\r\n\r\n"
                       "01234",
                       buffer, &length, &request));
    CHECK_INT(start_body(&request, buffer, &length), BODY_NEED_MORE);
    CHECK_VIEW(request.body, request.body_received, "01234");
    CHECK_INT(request.content_length - request.body_received, 11);
    destroy_request(&request);
}

/* B-3 (P11) */
static void test_b3_body_followed_by_next_request (void) {
    char buffer[MAX_HEADER_SIZE];
    size_t length = 0;
    request_t request = {0};

    BEGIN_TEST("B-3");
    CHECK(prepare_body("POST /a HTTP/1.1\r\nHost: x\r\n"
                       "Content-Length: 16\r\n\r\n"
                       "0123456789abcdef"
                       "GET /next HTTP/1.1\r\n",
                       buffer, &length, &request));
    CHECK_INT(start_body(&request, buffer, &length), BODY_DONE);
    CHECK_VIEW(request.body, request.body_received, "0123456789abcdef");
    CHECK_VIEW(buffer, length, "GET /next HTTP/1.1\r\n");
    destroy_request(&request);
}

/* B-4 (P11) */
static void test_b4_no_body_buffer (void) {
    char buffer[MAX_HEADER_SIZE];
    size_t length = 0;
    request_t zero_length = {0};
    request_t no_length = {0};

    BEGIN_TEST("B-4");
    CHECK(prepare_body("POST /a HTTP/1.1\r\nHost: x\r\n"
                       "Content-Length: 0\r\n\r\n",
                       buffer, &length, &zero_length));
    CHECK_INT(start_body(&zero_length, buffer, &length), BODY_DONE);
    CHECK(zero_length.body == NULL);

    CHECK(prepare_body("GET /a HTTP/1.1\r\nHost: x\r\n\r\n",
                       buffer, &length, &no_length));
    CHECK_INT(start_body(&no_length, buffer, &length), BODY_DONE);
    CHECK(no_length.body == NULL);
    destroy_request(&zero_length);
    destroy_request(&no_length);
}

/* B-5 (P10) */
static void test_b5_get_body_discarded (void) {
    char buffer[MAX_HEADER_SIZE];
    size_t length = 0;
    request_t request = {0};

    BEGIN_TEST("B-5");
    CHECK(prepare_body("GET /a HTTP/1.1\r\nHost: x\r\n"
                       "Content-Length: 5\r\n\r\n"
                       "hello"
                       "GET /next HTTP/1.1\r\n",
                       buffer, &length, &request));
    CHECK_INT(start_body(&request, buffer, &length), BODY_DONE);
    CHECK(request.body == NULL);
    CHECK_VIEW(buffer, length, "GET /next HTTP/1.1\r\n");
    destroy_request(&request);
}

/* B-6 (P26) 소켓이 본문 버퍼 빈 자리에 직접 읽은 것처럼 쓴다 */
static void test_b6_rest_of_body_written_directly (void) {
    char buffer[MAX_HEADER_SIZE];
    size_t length = 0;
    request_t request = {0};

    BEGIN_TEST("B-6");
    CHECK(prepare_body("POST /a HTTP/1.1\r\nHost: x\r\n"
                       "Content-Length: 16\r\n\r\n"
                       "01234",
                       buffer, &length, &request));
    CHECK_INT(start_body(&request, buffer, &length), BODY_NEED_MORE);
    CHECK(request.body != NULL);
    if (request.body == NULL) {
        return;
    }
    memcpy(request.body + request.body_received, "56789abcdef", 11);
    CHECK_INT(add_body_bytes(&request, 11), BODY_DONE);
    CHECK_VIEW(request.body, request.body_received, "0123456789abcdef");
    destroy_request(&request);
}

/* ── L. 수명과 메모리 ── */

/* L-1 (P14) */
static void test_l1_head_copy_survives_overwrite (void) {
    char buffer[MAX_HEADER_SIZE];
    size_t length = 0;
    request_t request = {0};

    BEGIN_TEST("L-1");
    load_buffer(buffer, &length, "GET /a HTTP/1.1\r\nHost: x\r\n\r\n");
    CHECK_INT(parse_bytes(buffer, length, &request), PARSE_OK);
    memset(buffer, 'Z', length);
    CHECK_VIEW(request.path, request.path_length, "/a");
    CHECK_VIEW(request.host, request.host_length, "x");
    destroy_request(&request);
}

/* L-2 (수명 규칙) */
static void test_l2_destroy_twice (void) {
    char buffer[MAX_HEADER_SIZE];
    size_t length = 0;
    request_t request = {0};

    BEGIN_TEST("L-2");
    CHECK(prepare_body("POST /a HTTP/1.1\r\nHost: x\r\n"
                       "Content-Length: 4\r\n\r\n"
                       "abcd",
                       buffer, &length, &request));
    CHECK_INT(start_body(&request, buffer, &length), BODY_DONE);
    destroy_request(&request);
    destroy_request(&request);
    CHECK(request.head == NULL);
    CHECK(request.body == NULL);
}

/* L-3 (Q5, P36) 누수·잘못된 접근은 make asan이 잡는다 */
static void test_l3_all_paths_release_memory (void) {
    char buffer[MAX_HEADER_SIZE];
    size_t length = 0;
    request_t normal = {0};
    request_t bad = {0};
    request_t cut_off = {0};

    BEGIN_TEST("L-3");
    CHECK(prepare_body("POST /a HTTP/1.1\r\nHost: x\r\n"
                       "Content-Length: 4\r\n\r\n"
                       "abcd",
                       buffer, &length, &normal));
    CHECK_INT(start_body(&normal, buffer, &length), BODY_DONE);
    destroy_request(&normal);

    CHECK_INT(parse_text("PUT /a HTTP/1.1\r\nHost: x\r\n\r\n", &bad),
              PARSE_BAD);
    destroy_request(&bad);

    CHECK(prepare_body("POST /a HTTP/1.1\r\nHost: x\r\n"
                       "Content-Length: 16\r\n\r\n"
                       "01234",
                       buffer, &length, &cut_off));
    CHECK_INT(start_body(&cut_off, buffer, &length), BODY_NEED_MORE);
    destroy_request(&cut_off);

    CHECK(normal.head == NULL && normal.body == NULL);
    CHECK(bad.head == NULL);
    CHECK(cut_off.head == NULL && cut_off.body == NULL);
}
