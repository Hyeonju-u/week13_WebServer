#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "http_request.h"
#include "http_response.h"
#include "static_file.h"
#include "check.h"

/* (S18, S32) 임시 디렉토리 안에 만든 것. 끝날 때 거꾸로 지운다 */
#define MAX_CREATED_PATHS 64

/* (R-5) 1994-11-06 08:49:37 UTC */
#define FIXED_NOW ((time_t) 784111777)

/* (O-1) file.html 내용의 길이 */
#define FILE_HTML_CONTENT "0123456789"

typedef struct {
    char path[PATH_MAX];
    bool is_directory;
} created_path_t;

typedef struct {
    int status;
    const char * reason;
} status_reason_t;

typedef struct {
    const char * path;
    const char * type;
} mime_case_t;

/* (E-2, R-6) 5번의 비-200 코드 전부 */
static const status_reason_t error_statuses[] = {
    { 301, "Moved Permanently" },
    { 400, "Bad Request" },
    { 403, "Forbidden" },
    { 404, "Not Found" },
    { 405, "Method Not Allowed" },
    { 411, "Length Required" },
    { 413, "Content Too Large" },
    { 414, "URI Too Long" },
    { 431, "Request Header Fields Too Large" },
    { 500, "Internal Server Error" },
    { 501, "Not Implemented" },
    { 505, "HTTP Version Not Supported" },
};

/* (M-1) 4번 MIME 표 전부 */
static const mime_case_t mime_table_cases[] = {
    { "a.html", "text/html; charset=utf-8" },
    { "a.htm", "text/html; charset=utf-8" },
    { "a.css", "text/css; charset=utf-8" },
    { "a.js", "text/javascript; charset=utf-8" },
    { "a.txt", "text/plain; charset=utf-8" },
    { "a.json", "application/json" },
    { "a.png", "image/png" },
    { "a.jpg", "image/jpeg" },
    { "a.jpeg", "image/jpeg" },
    { "a.gif", "image/gif" },
    { "a.svg", "image/svg+xml" },
    { "a.ico", "image/x-icon" },
    { "a.webp", "image/webp" },
    { "a.pdf", "application/pdf" },
    { "a.woff2", "font/woff2" },
    { "a.mp4", "video/mp4" },
};

static char base_directory[] = "/tmp/static_test_XXXXXX";
static char * static_root = NULL;
static created_path_t created_paths[MAX_CREATED_PATHS];
static size_t created_count = 0;

static bool create_fixture (void);
static bool make_directory (const char * relative);
static bool build_full_path (const char * relative, char * full_path);
static bool record_created (const char * full_path, bool is_directory);
static bool make_file (const char * relative, const char * content);
static bool make_locked_file (const char * relative, const char * content);
static bool change_mode (const char * relative, mode_t mode);
static bool make_link (const char * relative, const char * target);
static bool make_fifo (const char * relative);
static void destroy_fixture (void);
static void test_u1_encoded_space (void);
static void check_finds (const char * path, const char * relative);
static void open_path (method_t method, const char * path,
                       static_result_t * result);
static bool is_same_file (int fd, const char * relative);
static void test_u2_encoded_korean (void);
static void test_u3_query_dropped (void);
static void test_u4_query_not_checked (void);
static void check_status (method_t method, const char * path,
                          int expected_status);
static void test_u5_mixed_case_hex (void);
static void test_u6_broken_encoding (void);
static void test_u7_encoding_cut_at_end (void);
static void test_u8_encoded_nul (void);
static void test_u9_encoded_slash (void);
static void test_u10_decoded_only_once (void);
static void test_u11_plus_kept (void);
static void test_t1_dot_dot_inside (void);
static void test_t2_single_dot (void);
static void test_t3_above_root (void);
static void test_t4_above_root_after_down (void);
static void test_t5_encoded_dot_dot (void);
static void test_t6_above_root_missing_file (void);
static void test_t7_link_to_outside (void);
static void test_t8_link_to_inside (void);
static void test_t9_link_to_neighbor_prefix (void);
static void test_t10_dots_inside_name (void);
static void test_o1_existing_file (void);
static void test_o2_missing_file (void);
static void test_o3_file_used_as_directory (void);
static void test_o4_no_permission (void);
static void test_o5_fifo (void);
static void test_d1_root_index (void);
static void test_d2_directory_index (void);
static void test_d3_directory_without_slash (void);
static size_t write_head_text (const response_head_t * head, char * text);
static bool has_text (const char * text, const char * piece);
static void test_d4_directory_without_index (void);
static void test_d5_slash_checked_first (void);
static void test_d6_index_no_permission (void);
static void test_n1_post_existing_file (void);
static void check_method_not_allowed (const char * path);
static void test_n2_post_missing_file (void);
static void test_m1_mime_table (void);
static void check_mime (const char * path, const char * expected);
static void test_m2_extension_case (void);
static void test_m3_unknown_extension (void);
static void test_m4_no_extension (void);
static void test_m5_last_dot_only (void);
static void test_m6_dot_in_directory_name (void);
static void test_m7_no_charset_for_binary (void);
static void test_r1_ok_head (void);
static bool has_suffix (const char * text, size_t length,
                        const char * suffix);
static void test_r2_connection (void);
static void test_r3_allow_on_405 (void);
static void test_r4_location_on_301 (void);
static void test_r5_fixed_date (void);
static void test_r6_no_server_header (void);
static void test_e1_not_found_body (void);
static const char * find_body (const char * text);
static size_t read_content_length (const char * text);
static void test_e2_every_error_status (void);
static void test_e3_moved_body (void);
static void test_l1_close_ok_result (void);
static void test_l2_no_fd_left_on_errors (void);
static size_t count_open_fds (void);

/* ── 공개 함수 ── */

int main (void) {
    if (!create_fixture()) {
        printf("테스트 디렉토리를 만들지 못함\n");
        destroy_fixture();
        return 1;
    }

    test_u1_encoded_space();
    test_u2_encoded_korean();
    test_u3_query_dropped();
    test_u4_query_not_checked();
    test_u5_mixed_case_hex();
    test_u6_broken_encoding();
    test_u7_encoding_cut_at_end();
    test_u8_encoded_nul();
    test_u9_encoded_slash();
    test_u10_decoded_only_once();
    test_u11_plus_kept();

    test_t1_dot_dot_inside();
    test_t2_single_dot();
    test_t3_above_root();
    test_t4_above_root_after_down();
    test_t5_encoded_dot_dot();
    test_t6_above_root_missing_file();
    test_t7_link_to_outside();
    test_t8_link_to_inside();
    test_t9_link_to_neighbor_prefix();
    test_t10_dots_inside_name();

    test_o1_existing_file();
    test_o2_missing_file();
    test_o3_file_used_as_directory();
    test_o4_no_permission();
    test_o5_fifo();

    test_d1_root_index();
    test_d2_directory_index();
    test_d3_directory_without_slash();
    test_d4_directory_without_index();
    test_d5_slash_checked_first();
    test_d6_index_no_permission();

    test_n1_post_existing_file();
    test_n2_post_missing_file();

    test_m1_mime_table();
    test_m2_extension_case();
    test_m3_unknown_extension();
    test_m4_no_extension();
    test_m5_last_dot_only();
    test_m6_dot_in_directory_name();
    test_m7_no_charset_for_binary();

    test_r1_ok_head();
    test_r2_connection();
    test_r3_allow_on_405();
    test_r4_location_on_301();
    test_r5_fixed_date();
    test_r6_no_server_header();

    test_e1_not_found_body();
    test_e2_every_error_status();
    test_e3_moved_body();

    test_l1_close_ok_result();
    test_l2_no_fd_left_on_errors();
    /* L-3: 이 파일 전체를 make asan으로 돌린 결과로 확인한다 */

    destroy_fixture();
    return report_checks();
}

/* (S18) 루트는 base/www. 이웃 base/www2와 루트 밖 base/outside.txt를 같이 둔다 */
static bool create_fixture (void) {
    char root_path[PATH_MAX];

    if (mkdtemp(base_directory) == NULL) {
        return false;
    }
    bool is_made = make_directory("www") && make_directory("www2")
        && make_directory("www/a") && make_directory("www/docs")
        && make_directory("www/img")
        && make_file("outside.txt", "outside")
        && make_file("www2/f.html", "neighbor")
        && make_file("www/a b.html", "U-1")
        && make_file("www/한.html", "U-2")
        && make_file("www/a.html", "U-3")
        && make_file("www/secret.txt", "U-8")
        && make_file("www/a/b.html", "U-9")
        && make_file("www/%2e", "U-10")
        && make_file("www/a+b.html", "U-11")
        && make_file("www/b.html", "T-1")
        && make_file("www/a..b.html", "T-10")
        && make_file("www/file.html", FILE_HTML_CONTENT)
        && make_locked_file("www/locked.html", "O-4")
        && make_file("www/index.html", "D-1")
        && make_file("www/docs/index.html", "D-2")
        && make_link("www/link_out", "../outside.txt")
        && make_link("www/link_in", "b.html")
        && make_link("www/link_neighbor", "../www2/f.html")
        && make_fifo("www/fifo");
    if (!is_made || !build_full_path("www", root_path)) {
        return false;
    }
    static_root = copy_static_root(root_path);
    return static_root != NULL;
}

static bool make_directory (const char * relative) {
    char full_path[PATH_MAX];

    if (!build_full_path(relative, full_path)) {
        return false;
    }
    if (mkdir(full_path, 0755) != 0) {
        return false;
    }
    return record_created(full_path, true);
}

static bool build_full_path (const char * relative, char * full_path) {
    int written = snprintf(full_path, PATH_MAX, "%s/%s",
                           base_directory, relative);
    return written > 0 && written < PATH_MAX;
}

static bool record_created (const char * full_path, bool is_directory) {
    if (created_count == MAX_CREATED_PATHS) {
        return false;
    }
    created_path_t * slot = &created_paths[created_count];
    snprintf(slot->path, PATH_MAX, "%s", full_path);
    slot->is_directory = is_directory;
    created_count++;
    return true;
}

static bool make_file (const char * relative, const char * content) {
    char full_path[PATH_MAX];

    if (!build_full_path(relative, full_path)) {
        return false;
    }
    int fd = open(full_path, O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (fd < 0) {
        return false;
    }
    size_t length = strlen(content);
    bool is_written = write(fd, content, length) == (ssize_t) length;
    close(fd);
    return record_created(full_path, false) && is_written;
}

/* (O-4) 권한 000. 지울 때는 디렉토리 권한만 보므로 되돌리지 않는다 */
static bool make_locked_file (const char * relative, const char * content) {
    return make_file(relative, content) && change_mode(relative, 0);
}

static bool change_mode (const char * relative, mode_t mode) {
    char full_path[PATH_MAX];

    if (!build_full_path(relative, full_path)) {
        return false;
    }
    return chmod(full_path, mode) == 0;
}

static bool make_link (const char * relative, const char * target) {
    char full_path[PATH_MAX];

    if (!build_full_path(relative, full_path)) {
        return false;
    }
    if (symlink(target, full_path) != 0) {
        return false;
    }
    return record_created(full_path, false);
}

static bool make_fifo (const char * relative) {
    char full_path[PATH_MAX];

    if (!build_full_path(relative, full_path)) {
        return false;
    }
    if (mkfifo(full_path, 0644) != 0) {
        return false;
    }
    return record_created(full_path, false);
}

/* (S18, S32) 만든 순서의 거꾸로 지우면 디렉토리가 비어 있다 */
static void destroy_fixture (void) {
    free(static_root);
    static_root = NULL;
    while (created_count > 0) {
        created_count--;
        created_path_t * slot = &created_paths[created_count];
        if (slot->is_directory) {
            rmdir(slot->path);
        } else {
            unlink(slot->path);
        }
    }
    rmdir(base_directory);
}

/* U-1 (S2) */
static void test_u1_encoded_space (void) {
    BEGIN_TEST("U-1");
    check_finds("/a%20b.html", "www/a b.html");
}

/* (S32) 200이고 열린 fd가 relative 파일과 같은 파일인지 본다 */
static void check_finds (const char * path, const char * relative) {
    static_result_t result;

    open_path(METHOD_GET, path, &result);
    CHECK_INT(result.head.status, 200);
    CHECK(is_same_file(result.fd, relative));
    close_static_file(&result);
}

/* (S32) request_t는 직접 채운다. path는 문자열 상수라 result보다 오래 산다 */
static void open_path (method_t method, const char * path,
                       static_result_t * result) {
    request_t request = {0};

    request.method = method;
    request.path = path;
    request.path_length = strlen(path);
    request.keep_alive = true;
    open_static_file(&request, static_root, result);
}

/* (S32) 장치 번호와 inode 번호가 모두 같으면 같은 파일 */
static bool is_same_file (int fd, const char * relative) {
    char full_path[PATH_MAX];
    struct stat opened;
    struct stat expected;

    if (fd < 0 || !build_full_path(relative, full_path)) {
        return false;
    }
    if (fstat(fd, &opened) != 0 || stat(full_path, &expected) != 0) {
        return false;
    }
    return opened.st_dev == expected.st_dev
        && opened.st_ino == expected.st_ino;
}

/* U-2 (S2) */
static void test_u2_encoded_korean (void) {
    BEGIN_TEST("U-2");
    check_finds("/%ED%95%9C.html", "www/한.html");
}

/* U-3 (S1) */
static void test_u3_query_dropped (void) {
    BEGIN_TEST("U-3");
    check_finds("/a.html?x=1", "www/a.html");
}

/* U-4 (S1, S3) */
static void test_u4_query_not_checked (void) {
    BEGIN_TEST("U-4");
    check_status(METHOD_GET, "/a.html?x=%zz", 200);
}

static void check_status (method_t method, const char * path,
                          int expected_status) {
    static_result_t result;

    open_path(method, path, &result);
    CHECK_INT(result.head.status, expected_status);
    close_static_file(&result);
}

/* U-5 (S2) `/%2E%2e`는 `/..`로 풀려 루트 위가 되므로 403으로 확인한다 (S6) */
static void test_u5_mixed_case_hex (void) {
    BEGIN_TEST("U-5");
    check_finds("/%61.html", "www/a.html");
    check_status(METHOD_GET, "/%2E%2e", 403);
}

/* U-6 (S3) */
static void test_u6_broken_encoding (void) {
    BEGIN_TEST("U-6");
    check_status(METHOD_GET, "/a%zz.html", 400);
}

/* U-7 (S3) */
static void test_u7_encoding_cut_at_end (void) {
    BEGIN_TEST("U-7");
    check_status(METHOD_GET, "/a%2", 400);
    check_status(METHOD_GET, "/a%", 400);
}

/* U-8 (S4) */
static void test_u8_encoded_nul (void) {
    BEGIN_TEST("U-8");
    check_status(METHOD_GET, "/secret.txt%00.html", 400);
}

/* U-9 (S5) www/a/b.html이 실제로 있어도 404 */
static void test_u9_encoded_slash (void) {
    BEGIN_TEST("U-9");
    check_status(METHOD_GET, "/a%2Fb.html", 404);
    check_status(METHOD_GET, "/a%2fb.html", 404);
}

/* U-10 (S2) */
static void test_u10_decoded_only_once (void) {
    BEGIN_TEST("U-10");
    check_finds("/%252e", "www/%2e");
}

/* U-11 (S2) */
static void test_u11_plus_kept (void) {
    BEGIN_TEST("U-11");
    check_finds("/a+b.html", "www/a+b.html");
}

/* T-1 (S6) */
static void test_t1_dot_dot_inside (void) {
    BEGIN_TEST("T-1");
    check_finds("/a/../b.html", "www/b.html");
}

/* T-2 (S6) */
static void test_t2_single_dot (void) {
    BEGIN_TEST("T-2");
    check_finds("/./b.html", "www/b.html");
}

/* T-3 (S6) */
static void test_t3_above_root (void) {
    BEGIN_TEST("T-3");
    check_status(METHOD_GET, "/../etc/passwd", 403);
}

/* T-4 (S6) */
static void test_t4_above_root_after_down (void) {
    BEGIN_TEST("T-4");
    check_status(METHOD_GET, "/a/../../b.html", 403);
}

/* T-5 (S2, S6) */
static void test_t5_encoded_dot_dot (void) {
    BEGIN_TEST("T-5");
    check_status(METHOD_GET, "/%2e%2e/secret", 403);
}

/* T-6 (S6) 404가 아니라 403 */
static void test_t6_above_root_missing_file (void) {
    BEGIN_TEST("T-6");
    check_status(METHOD_GET, "/../없는파일", 403);
}

/* T-7 (S6) www/link_out → base/outside.txt */
static void test_t7_link_to_outside (void) {
    BEGIN_TEST("T-7");
    check_status(METHOD_GET, "/link_out", 403);
}

/* T-8 (S6) www/link_in → www/b.html */
static void test_t8_link_to_inside (void) {
    BEGIN_TEST("T-8");
    check_finds("/link_in", "www/b.html");
}

/* T-9 (S6) www/link_neighbor → base/www2/f.html (접두어만 같음) */
static void test_t9_link_to_neighbor_prefix (void) {
    BEGIN_TEST("T-9");
    check_status(METHOD_GET, "/link_neighbor", 403);
}

/* T-10 (S6) */
static void test_t10_dots_inside_name (void) {
    BEGIN_TEST("T-10");
    check_finds("/a..b.html", "www/a..b.html");
}

/* O-1 (S11) */
static void test_o1_existing_file (void) {
    static_result_t result;

    BEGIN_TEST("O-1");
    open_path(METHOD_GET, "/file.html", &result);
    CHECK_INT(result.head.status, 200);
    CHECK(result.fd >= 0 && fcntl(result.fd, F_GETFD) != -1);
    CHECK_INT(result.head.content_length, strlen(FILE_HTML_CONTENT));
    CHECK(is_same_file(result.fd, "www/file.html"));
    close_static_file(&result);
}

/* O-2 (S7) */
static void test_o2_missing_file (void) {
    BEGIN_TEST("O-2");
    check_status(METHOD_GET, "/missing.html", 404);
}

/* O-3 (S7) ENOTDIR */
static void test_o3_file_used_as_directory (void) {
    BEGIN_TEST("O-3");
    check_status(METHOD_GET, "/file.html/x", 404);
}

/* O-4 (S7, S18) root로 돌리면 열려서 실패한다 */
static void test_o4_no_permission (void) {
    BEGIN_TEST("O-4");
    check_status(METHOD_GET, "/locked.html", 403);
}

/* O-5 (S7) 열기 전에 걸러야 여기서 멈추지 않는다 */
static void test_o5_fifo (void) {
    BEGIN_TEST("O-5");
    check_status(METHOD_GET, "/fifo", 403);
}

/* D-1 (S10) */
static void test_d1_root_index (void) {
    BEGIN_TEST("D-1");
    check_finds("/", "www/index.html");
}

/* D-2 (S10) */
static void test_d2_directory_index (void) {
    BEGIN_TEST("D-2");
    check_finds("/docs/", "www/docs/index.html");
}

/* D-3 (S9, S22) */
static void test_d3_directory_without_slash (void) {
    static_result_t result;
    char text[MAX_RESPONSE_HEAD_SIZE + 1];

    BEGIN_TEST("D-3");
    open_path(METHOD_GET, "/docs", &result);
    CHECK_INT(result.head.status, 301);
    write_head_text(&result.head, text);
    CHECK(has_text(text, "\r\nLocation: /docs/\r\n"));
    close_static_file(&result);
}

/* (S28) 고정 시각으로 쓰고 끝에 '\0'을 붙인다. 반환: 쓴 바이트 수 */
static size_t write_head_text (const response_head_t * head, char * text) {
    size_t length = write_response_head(head, FIXED_NOW, text);

    CHECK(length <= MAX_RESPONSE_HEAD_SIZE);
    if (length > MAX_RESPONSE_HEAD_SIZE) {
        length = MAX_RESPONSE_HEAD_SIZE;
    }
    text[length] = '\0';
    return length;
}

static bool has_text (const char * text, const char * piece) {
    return strstr(text, piece) != NULL;
}

/* D-4 (S10) */
static void test_d4_directory_without_index (void) {
    BEGIN_TEST("D-4");
    check_status(METHOD_GET, "/img/", 403);
}

/* D-5 (S9) */
static void test_d5_slash_checked_first (void) {
    BEGIN_TEST("D-5");
    check_status(METHOD_GET, "/img", 301);
}

/* D-6 (S7, S10) D-2의 docs/index.html을 잠시 000으로 바꾼다 */
static void test_d6_index_no_permission (void) {
    BEGIN_TEST("D-6");
    CHECK(change_mode("www/docs/index.html", 0));
    check_status(METHOD_GET, "/docs/", 403);
    CHECK(change_mode("www/docs/index.html", 0644));
}

/* N-1 (S8) */
static void test_n1_post_existing_file (void) {
    BEGIN_TEST("N-1");
    check_method_not_allowed("/b.html");
}

static void check_method_not_allowed (const char * path) {
    static_result_t result;
    char text[MAX_RESPONSE_HEAD_SIZE + 1];

    open_path(METHOD_POST, path, &result);
    CHECK_INT(result.head.status, 405);
    write_head_text(&result.head, text);
    CHECK(has_text(text, "\r\nAllow: GET\r\n"));
    close_static_file(&result);
}

/* N-2 (S8, S20) 파일 확인보다 먼저 405 */
static void test_n2_post_missing_file (void) {
    BEGIN_TEST("N-2");
    check_method_not_allowed("/없는파일");
}

/* M-1 (S14) */
static void test_m1_mime_table (void) {
    size_t count = sizeof(mime_table_cases) / sizeof(mime_table_cases[0]);

    BEGIN_TEST("M-1");
    for (size_t i = 0; i < count; i++) {
        check_mime(mime_table_cases[i].path, mime_table_cases[i].type);
    }
}

static void check_mime (const char * path, const char * expected) {
    const char * type = find_mime_type(path, strlen(path));

    CHECK(type != NULL);
    if (type == NULL) {
        return;
    }
    CHECK_VIEW(type, strlen(type), expected);
}

/* M-2 (S15) */
static void test_m2_extension_case (void) {
    BEGIN_TEST("M-2");
    check_mime("a.JPG", "image/jpeg");
    check_mime("a.Html", "text/html; charset=utf-8");
}

/* M-3 (S17) */
static void test_m3_unknown_extension (void) {
    BEGIN_TEST("M-3");
    check_mime("a.hwp", "application/octet-stream");
}

/* M-4 (S17) */
static void test_m4_no_extension (void) {
    BEGIN_TEST("M-4");
    check_mime("README", "application/octet-stream");
}

/* M-5 (S15, S17) */
static void test_m5_last_dot_only (void) {
    BEGIN_TEST("M-5");
    check_mime("a.tar.gz", "application/octet-stream");
}

/* M-6 (S15) */
static void test_m6_dot_in_directory_name (void) {
    BEGIN_TEST("M-6");
    check_mime("/v1.2/readme", "application/octet-stream");
}

/* M-7 (S16) */
static void test_m7_no_charset_for_binary (void) {
    BEGIN_TEST("M-7");
    check_mime("a.json", "application/json");
    check_mime("a.png", "image/png");
}

/* R-1 (S12) */
static void test_r1_ok_head (void) {
    response_head_t head = {0};
    char text[MAX_RESPONSE_HEAD_SIZE + 1];

    BEGIN_TEST("R-1");
    head.status = 200;
    head.keep_alive = true;
    head.content_type = "text/html; charset=utf-8";
    head.content_length = 1234;
    size_t length = write_head_text(&head, text);
    CHECK(strncmp(text, "HTTP/1.1 200 OK\r\n", 17) == 0);
    CHECK(has_text(text, "\r\nContent-Type: text/html; charset=utf-8\r\n"));
    CHECK(has_text(text, "\r\nContent-Length: 1234\r\n"));
    CHECK(has_text(text, "\r\nConnection: "));
    CHECK(has_text(text, "\r\nDate: "));
    CHECK(has_suffix(text, length, "\r\n\r\n"));
}

static bool has_suffix (const char * text, size_t length,
                        const char * suffix) {
    size_t suffix_length = strlen(suffix);

    if (length < suffix_length) {
        return false;
    }
    return memcmp(text + length - suffix_length, suffix, suffix_length) == 0;
}

/* R-2 (S12) */
static void test_r2_connection (void) {
    response_head_t head = {0};
    char text[MAX_RESPONSE_HEAD_SIZE + 1];

    BEGIN_TEST("R-2");
    head.status = 200;
    head.content_type = "text/plain; charset=utf-8";
    head.keep_alive = true;
    write_head_text(&head, text);
    CHECK(has_text(text, "\r\nConnection: keep-alive\r\n"));
    head.keep_alive = false;
    write_head_text(&head, text);
    CHECK(has_text(text, "\r\nConnection: close\r\n"));
}

/* R-3 (S8, S12) */
static void test_r3_allow_on_405 (void) {
    response_head_t head = {0};
    char text[MAX_RESPONSE_HEAD_SIZE + 1];

    BEGIN_TEST("R-3");
    head.status = 405;
    write_head_text(&head, text);
    CHECK(has_text(text, "\r\nAllow: GET\r\n"));
}

/* R-4 (S9, S12, S22) location에는 '/'를 붙이기 전 원문이 들어 있다 */
static void test_r4_location_on_301 (void) {
    response_head_t head = {0};
    char text[MAX_RESPONSE_HEAD_SIZE + 1];

    BEGIN_TEST("R-4");
    head.status = 301;
    head.location = "/docs";
    head.location_length = strlen("/docs");
    write_head_text(&head, text);
    CHECK(has_text(text, "\r\nLocation: /docs/\r\n"));
}

/* R-5 (S12, S28) */
static void test_r5_fixed_date (void) {
    response_head_t head = {0};
    char text[MAX_RESPONSE_HEAD_SIZE + 1];

    BEGIN_TEST("R-5");
    head.status = 200;
    head.content_type = "text/html; charset=utf-8";
    write_head_text(&head, text);
    CHECK(has_text(text, "\r\nDate: Sun, 06 Nov 1994 08:49:37 GMT\r\n"));
}

/* R-6 (S12) 200과 5번의 비-200 코드 전부 */
static void test_r6_no_server_header (void) {
    size_t count = sizeof(error_statuses) / sizeof(error_statuses[0]);
    response_head_t head = {0};
    char text[MAX_RESPONSE_HEAD_SIZE + 1];

    BEGIN_TEST("R-6");
    head.status = 200;
    head.content_type = "text/html; charset=utf-8";
    write_head_text(&head, text);
    CHECK(!has_text(text, "\r\nServer:"));
    head.location = "/docs";
    head.location_length = strlen("/docs");
    for (size_t i = 0; i < count; i++) {
        head.status = error_statuses[i].status;
        write_head_text(&head, text);
        CHECK(!has_text(text, "\r\nServer:"));
    }
}

/* E-1 (S13) */
static void test_e1_not_found_body (void) {
    response_head_t head = {0};
    char text[MAX_RESPONSE_HEAD_SIZE + 1];

    BEGIN_TEST("E-1");
    head.status = 404;
    size_t length = write_head_text(&head, text);
    CHECK(has_text(text, "\r\nContent-Type: text/html; charset=utf-8\r\n"));
    const char * body = find_body(text);
    CHECK(body != NULL);
    if (body == NULL) {
        return;
    }
    size_t body_length = length - (size_t) (body - text);
    CHECK(body_length > 0);
    CHECK_INT(read_content_length(text), body_length);
}

/* 반환: 헤더 끝의 빈 줄 바로 뒤, NULL = 빈 줄 없음 */
static const char * find_body (const char * text) {
    const char * end = strstr(text, "\r\n\r\n");

    if (end == NULL) {
        return NULL;
    }
    return end + 4;
}

/* 반환: Content-Length 값, 줄이 없으면 0 */
static size_t read_content_length (const char * text) {
    const char * name = "\r\nContent-Length: ";
    const char * line = strstr(text, name);

    if (line == NULL) {
        return 0;
    }
    return (size_t) strtoul(line + strlen(name), NULL, 10);
}

/* E-2 (S13) 301은 Location이 있어야 하니 같이 채운다 */
static void test_e2_every_error_status (void) {
    size_t count = sizeof(error_statuses) / sizeof(error_statuses[0]);
    response_head_t head = {0};
    char text[MAX_RESPONSE_HEAD_SIZE + 1];
    char status_line[64];

    BEGIN_TEST("E-2");
    head.location = "/docs";
    head.location_length = strlen("/docs");
    for (size_t i = 0; i < count; i++) {
        head.status = error_statuses[i].status;
        snprintf(status_line, sizeof(status_line), "HTTP/1.1 %d %s\r\n",
                 error_statuses[i].status, error_statuses[i].reason);
        size_t length = write_head_text(&head, text);
        CHECK(strncmp(text, status_line, strlen(status_line)) == 0);
        const char * body = find_body(text);
        CHECK(body != NULL && (size_t) (body - text) < length);
    }
}

/* E-3 (S13, S19) */
static void test_e3_moved_body (void) {
    response_head_t head = {0};
    char text[MAX_RESPONSE_HEAD_SIZE + 1];

    BEGIN_TEST("E-3");
    head.status = 301;
    head.location = "/docs";
    head.location_length = strlen("/docs");
    write_head_text(&head, text);
    CHECK(has_text(text, "\r\nLocation: /docs/\r\n"));
    const char * body = find_body(text);
    CHECK(body != NULL && body[0] != '\0');
    CHECK(body != NULL && !has_text(body, "/docs/"));
}

/* L-1 (수명 규칙) */
static void test_l1_close_ok_result (void) {
    static_result_t result;

    BEGIN_TEST("L-1");
    open_path(METHOD_GET, "/b.html", &result);
    CHECK_INT(result.head.status, 200);
    int fd = result.fd;
    close_static_file(&result);
    CHECK(fcntl(fd, F_GETFD) == -1);
}

/* L-2 (수명 규칙) 403, 404, 301, 405 */
static void test_l2_no_fd_left_on_errors (void) {
    BEGIN_TEST("L-2");
    size_t before = count_open_fds();
    check_status(METHOD_GET, "/locked.html", 403);
    check_status(METHOD_GET, "/missing.html", 404);
    check_status(METHOD_GET, "/docs", 301);
    check_status(METHOD_POST, "/b.html", 405);
    CHECK_INT(count_open_fds(), before);
}

/* (S32) /proc/self/fd 항목 수. 세는 동안 opendir이 연 fd도 늘 하나 들어간다 */
static size_t count_open_fds (void) {
    DIR * directory = opendir("/proc/self/fd");
    size_t count = 0;

    if (directory == NULL) {
        return 0;
    }
    while (readdir(directory) != NULL) {
        count++;
    }
    closedir(directory);
    return count;
}
