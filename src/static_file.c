#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "static_file.h"

/* (S17) */
#define DEFAULT_MIME_TYPE "application/octet-stream"

/* (S10) */
#define INDEX_FILE_NAME "index.html"

typedef struct {
    const char * extension;
    const char * type;
} mime_entry_t;

/* (S14, S16) 4번 표 */
static const mime_entry_t mime_table[] = {
    { "html", "text/html; charset=utf-8" },
    { "htm", "text/html; charset=utf-8" },
    { "css", "text/css; charset=utf-8" },
    { "js", "text/javascript; charset=utf-8" },
    { "txt", "text/plain; charset=utf-8" },
    { "json", "application/json" },
    { "png", "image/png" },
    { "jpg", "image/jpeg" },
    { "jpeg", "image/jpeg" },
    { "gif", "image/gif" },
    { "svg", "image/svg+xml" },
    { "ico", "image/x-icon" },
    { "webp", "image/webp" },
    { "pdf", "application/pdf" },
    { "woff2", "font/woff2" },
    { "mp4", "video/mp4" },
};

static void reset_result (static_result_t * result,
                          const request_t * request);
static size_t find_query_start (const char * path, size_t length);
static int decode_path (const char * source, size_t length, char * decoded);
static int decode_next_byte (const char * source, size_t length,
                             size_t * position, char * byte);
static bool is_control_byte (unsigned char letter);
static int read_hex_pair (const char * source, size_t length, size_t start);
static int read_hex_digit (char letter);
static bool normalize_dots (const char * path, char * normalized);
static bool apply_segment (const char * segment, size_t segment_length,
                           char * normalized, size_t * length);
static bool is_segment (const char * segment, size_t segment_length,
                        const char * expected);
static bool remove_last_segment (const char * normalized, size_t * length);
static bool is_name_segment (const char * segment, size_t segment_length);
static int find_target_file (const char * root, const char * normalized,
                             char * file_path, char * real_path);
static bool join_path (const char * root, const char * normalized,
                       char * file_path);
static int check_path (const char * file_path, const char * root,
                       char * real_path, struct stat * info);
static int map_file_error (int error);
static bool is_inside_root (const char * real_path, const char * root);
static int find_index_file (const char * root, const char * normalized,
                            char * file_path, char * real_path);
static bool has_trailing_slash (const char * normalized);
static bool append_index_name (char * file_path);
static int check_file_type (const struct stat * info);
static void set_location (static_result_t * result, const char * path,
                          size_t length);
static int open_found_file (const char * real_path,
                            static_result_t * result);
static bool find_extension (const char * path, size_t length,
                            const char ** extension,
                            size_t * extension_length);
static bool is_extension_equal (const char * extension, size_t length,
                                const char * expected);

/* ── 공개 함수 ── */

/* (R1d, S26) */
char * copy_static_root (const char * path) {
    return realpath(path, NULL);
}

/* (R1d, R1g, R4c) 검사 순서는 step2-static.md 3번 표. 먼저 걸린 것에서 멈춘다 */
void open_static_file (const request_t * request, const char * root,
                       static_result_t * result) {
    char normalized[MAX_PATH_TEXT_SIZE];
    char file_path[PATH_MAX];
    char real_path[PATH_MAX];
    size_t path_length = find_query_start(request->path,
                                          request->path_length);

    reset_result(result, request);
    result->head.status = normalize_request_path(request->path,
                                                 request->path_length,
                                                 normalized);
    if (result->head.status != 200) {
        return;
    }
    if (request->method == METHOD_POST) {
        result->head.status = 405;
        return;
    }
    result->head.status = find_target_file(root, normalized, file_path,
                                           real_path);
    if (result->head.status == 301) {
        set_location(result, request->path, path_length);
        return;
    }
    if (result->head.status != 200) {
        return;
    }
    result->head.status = open_found_file(real_path, result);
    if (result->head.status != 200) {
        return;
    }
    result->head.content_type = find_mime_type(file_path,
                                               strlen(file_path));
}

/* (S1~S6, S24, G3, G32) 검사 순서 1~3번만 (파일시스템은 보지 않는다) */
int normalize_request_path (const char * path, size_t length,
                            char * normalized) {
    char decoded[MAX_PATH_TEXT_SIZE];
    size_t path_length = find_query_start(path, length);

    int status = decode_path(path, path_length, decoded);
    if (status != 200) {
        return status;
    }
    if (!normalize_dots(decoded, normalized)) {
        return 403;
    }
    return 200;
}

/* (R1e, S14, S15, S17) */
const char * find_mime_type (const char * path, size_t length) {
    const char * extension = NULL;
    size_t extension_length = 0;
    size_t count = sizeof(mime_table) / sizeof(mime_table[0]);

    if (!find_extension(path, length, &extension, &extension_length)) {
        return DEFAULT_MIME_TYPE;
    }
    for (size_t i = 0; i < count; i++) {
        if (is_extension_equal(extension, extension_length,
                               mime_table[i].extension)) {
            return mime_table[i].type;
        }
    }
    return DEFAULT_MIME_TYPE;
}

/* (S11, 수명 규칙) */
void close_static_file (static_result_t * result) {
    if (result->fd >= 0) {
        close(result->fd);
    }
    result->fd = -1;
}

/* ── 도우미 함수 ── */

/* (S25) 0으로 채운 fd(표준 입력)를 닫지 않도록 맨 먼저 -1로 둔다 */
static void reset_result (static_result_t * result,
                          const request_t * request) {
    result->head = (response_head_t) {0};
    result->head.keep_alive = request->keep_alive;
    result->fd = -1;
}

/* (S1) 반환: '?' 앞까지의 길이 */
static size_t find_query_start (const char * path, size_t length) {
    const char * question = memchr(path, '?', length);

    if (question == NULL) {
        return length;
    }
    return (size_t) (question - path);
}

/* (S2~S5, S24) 앞에서부터 보고 처음 걸린 것으로 답한다.
 * decoded: MAX_PATH_TEXT_SIZE. 반환: 200 = 성공, 그 밖 = 응답 코드 */
static int decode_path (const char * source, size_t length, char * decoded) {
    size_t copy_length = 0;

    for (size_t i = 0; i < length; i++) {
        int status = decode_next_byte(source, length, &i,
                                      &decoded[copy_length]);
        if (status != 200) {
            return status;
        }
        copy_length++;
    }
    decoded[copy_length] = '\0';
    return 200;
}

/* (S2~S5, S24) %XX면 *position을 마지막 16진수 자리로 옮긴다 */
static int decode_next_byte (const char * source, size_t length,
                             size_t * position, char * byte) {
    unsigned char letter = (unsigned char) source[*position];

    if (is_control_byte(letter)) {
        return 400;
    }
    if (letter != '%') {
        *byte = (char) letter;
        return 200;
    }
    int value = read_hex_pair(source, length, *position + 1);
    /* (S3) 깨진 인코딩, (S4) %00 */
    if (value <= 0) {
        return 400;
    }
    if (value == '/') {
        return 404;
    }
    *byte = (char) value;
    *position += 2;
    return 200;
}

/* (S24) */
static bool is_control_byte (unsigned char letter) {
    return letter < 0x20 || letter == 0x7F;
}

/* (S2, S3) 반환: 0~255, -1 = 16진수 두 자리가 아님 */
static int read_hex_pair (const char * source, size_t length, size_t start) {
    if (start + 2 > length) {
        return -1;
    }
    int high = read_hex_digit(source[start]);
    int low = read_hex_digit(source[start + 1]);
    if (high < 0 || low < 0) {
        return -1;
    }
    return high * 16 + low;
}

/* (S2) 대소문자 모두 받는다. 반환: 0~15, -1 = 16진수 아님 */
static int read_hex_digit (char letter) {
    if (letter >= '0' && letter <= '9') {
        return letter - '0';
    }
    if (letter >= 'a' && letter <= 'f') {
        return letter - 'a' + 10;
    }
    if (letter >= 'A' && letter <= 'F') {
        return letter - 'A' + 10;
    }
    return -1;
}

/* (S6) 글자로만 정리한다. 마지막이 '/', '.', '..'면 끝에 '/'를 남긴다.
 * normalized: MAX_PATH_TEXT_SIZE. 반환: false = 루트 위로 올라감 */
static bool normalize_dots (const char * path, char * normalized) {
    const char * segment = path;
    size_t length = 0;
    bool is_directory_end = true;

    while (*segment != '\0') {
        segment += strspn(segment, "/");
        size_t segment_length = strcspn(segment, "/");
        if (!apply_segment(segment, segment_length, normalized, &length)) {
            return false;
        }
        is_directory_end = !is_name_segment(segment, segment_length)
            || segment[segment_length] == '/';
        segment += segment_length;
    }
    if (length == 0 || is_directory_end) {
        normalized[length++] = '/';
    }
    normalized[length] = '\0';
    return true;
}

/* (S6) 반환: false = 루트 위의 '..' */
static bool apply_segment (const char * segment, size_t segment_length,
                           char * normalized, size_t * length) {
    if (segment_length == 0 || is_segment(segment, segment_length, ".")) {
        return true;
    }
    if (is_segment(segment, segment_length, "..")) {
        return remove_last_segment(normalized, length);
    }
    normalized[*length] = '/';
    memcpy(normalized + *length + 1, segment, segment_length);
    *length += segment_length + 1;
    return true;
}

static bool is_segment (const char * segment, size_t segment_length,
                        const char * expected) {
    return segment_length == strlen(expected)
        && memcmp(segment, expected, segment_length) == 0;
}

/* (S6) 반환: false = 지울 조각이 없음 (루트 위) */
static bool remove_last_segment (const char * normalized, size_t * length) {
    if (*length == 0) {
        return false;
    }
    while (normalized[*length - 1] != '/') {
        (*length)--;
    }
    (*length)--;
    return true;
}

/* (S6) '.'·'..'·빈 조각이 아닌 이름 */
static bool is_name_segment (const char * segment, size_t segment_length) {
    return segment_length > 0 && !is_segment(segment, segment_length, ".")
        && !is_segment(segment, segment_length, "..");
}

/* (S6, S7, S9, S10, S21) 검사 순서 4~7번.
 * 반환: 200 = real_path가 루트 안의 일반 파일, 그 밖 = 응답 코드 */
static int find_target_file (const char * root, const char * normalized,
                             char * file_path, char * real_path) {
    struct stat info;

    if (!join_path(root, normalized, file_path)) {
        return 404;
    }
    int status = check_path(file_path, root, real_path, &info);
    if (status != 200) {
        return status;
    }
    if (S_ISDIR(info.st_mode)) {
        return find_index_file(root, normalized, file_path, real_path);
    }
    return check_file_type(&info);
}

/* (S21) PATH_MAX를 넘으면 ENAMETOOLONG과 같게 본다.
 * 반환: false = 너무 김 */
static bool join_path (const char * root, const char * normalized,
                       char * file_path) {
    size_t root_length = strlen(root);
    size_t path_length = strlen(normalized);

    if (root_length + path_length + 1 > PATH_MAX) {
        return false;
    }
    memcpy(file_path, root, root_length);
    memcpy(file_path + root_length, normalized, path_length + 1);
    return true;
}

/* (S6, S7, S21) 검사 순서 4번(stat) → 5번(realpath 경계) */
static int check_path (const char * file_path, const char * root,
                       char * real_path, struct stat * info) {
    if (stat(file_path, info) != 0) {
        return map_file_error(errno);
    }
    if (realpath(file_path, real_path) == NULL) {
        return map_file_error(errno);
    }
    if (!is_inside_root(real_path, root)) {
        return 403;
    }
    return 200;
}

/* (S7, S21) */
static int map_file_error (int error) {
    if (error == ENOENT || error == ENOTDIR || error == ENAMETOOLONG) {
        return 404;
    }
    if (error == EACCES || error == ELOOP) {
        return 403;
    }
    return 500;
}

/* (S6, S26) 루트 바로 뒤가 '/'이거나 끝이어야 안쪽이다 (T-9).
 * 루트가 '/'면 '/'로 시작하는 모든 경로가 안쪽 */
static bool is_inside_root (const char * real_path, const char * root) {
    size_t root_length = strlen(root);

    if (strncmp(real_path, root, root_length) != 0) {
        return false;
    }
    if (root_length > 0 && root[root_length - 1] == '/') {
        return true;
    }
    return real_path[root_length] == '/' || real_path[root_length] == '\0';
}

/* (S9, S10) index.html도 4·5·7번 검사를 받는다. 없으면 403 */
static int find_index_file (const char * root, const char * normalized,
                            char * file_path, char * real_path) {
    struct stat info;

    if (!has_trailing_slash(normalized)) {
        return 301;
    }
    if (!append_index_name(file_path)) {
        return 403;
    }
    int status = check_path(file_path, root, real_path, &info);
    if (status == 404) {
        return 403;
    }
    if (status != 200) {
        return status;
    }
    return check_file_type(&info);
}

static bool has_trailing_slash (const char * normalized) {
    size_t length = strlen(normalized);

    return length > 0 && normalized[length - 1] == '/';
}

/* (S10) file_path는 '/'로 끝난다. 반환: false = PATH_MAX를 넘음 */
static bool append_index_name (char * file_path) {
    size_t length = strlen(file_path);
    size_t name_length = strlen(INDEX_FILE_NAME);

    if (length + name_length + 1 > PATH_MAX) {
        return false;
    }
    memcpy(file_path + length, INDEX_FILE_NAME, name_length + 1);
    return true;
}

/* (S7) FIFO는 열면 멈출 수 있어 열기 전에 거른다 (O-5) */
static int check_file_type (const struct stat * info) {
    if (!S_ISREG(info->st_mode)) {
        return 403;
    }
    return 200;
}

/* (S22) 원문 경로를 빌린다. '/'는 write_response_head가 붙인다 */
static void set_location (static_result_t * result, const char * path,
                          size_t length) {
    result->head.location = path;
    result->head.location_length = length;
}

/* (S6, S7, S11) 경계 검사를 받은 실제 경로(real_path)를 연다.
 * 링크를 다시 따라가지 않도록. 반환: 200 / 403 / 500 */
static int open_found_file (const char * real_path,
                            static_result_t * result) {
    struct stat info;
    int fd = open(real_path, O_RDONLY);

    if (fd < 0) {
        return errno == EACCES ? 403 : 500;
    }
    if (fstat(fd, &info) != 0) {
        close(fd);
        return 500;
    }
    result->fd = fd;
    result->head.content_length = (size_t) info.st_size;
    return 200;
}

/* (S15) 마지막 '/' 뒤 조각의 마지막 '.' 뒤.
 * 반환: false = 확장자 없음 */
static bool find_extension (const char * path, size_t length,
                            const char ** extension,
                            size_t * extension_length) {
    for (size_t i = length; i > 0; i--) {
        if (path[i - 1] == '/') {
            return false;
        }
        if (path[i - 1] == '.') {
            *extension = path + i;
            *extension_length = length - i;
            return true;
        }
    }
    return false;
}

/* (S15) 대소문자 무시 */
static bool is_extension_equal (const char * extension, size_t length,
                                const char * expected) {
    return length == strlen(expected)
        && strncasecmp(extension, expected, length) == 0;
}
