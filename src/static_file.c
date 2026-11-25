#include "static_file.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "mime.h"

#ifndef NAME_MAX
#define NAME_MAX 255
#endif

/*
 * 링크는 따라가지 않고(O_NOFOLLOW), FIFO·장치를 열어도 멈추지 않게 한다
 * (O_NONBLOCK). 한 칸씩 열어서 검사와 열기 사이에 바꿔치기할 틈이 없다.
 */
#define OPEN_FLAGS (O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC)

static int check_path_components (size_t root_path_length, const char * path);
static int walk_path (int root_fd, const char * path, int * error_number);
static int open_next_component (int directory_fd, const char * name,
    size_t name_length, int * error_number);
static int convert_open_error (int error_number);
static int prepare_target (int target_fd, const char * path,
    static_file_t * file);
static int prepare_directory (int directory_fd, const char * path,
    static_file_t * file);
static int has_trailing_slash (const char * path);
static int convert_index_error (int error_number);
static int fill_regular_file (int file_fd, const char * name,
    static_file_t * file);

/* ── 공개 함수 ── */

/*
 * 반환값은 HTTP 상태 코드다. 200이면 file이 채워지고 호출자가 fd를 닫는다.
 * 301은 폴더 요청 경로 끝에 /가 없다는 뜻이고, file은 비어 있다.
 * root_fd는 빌려 쓰는 것이라 닫지 않는다.
 */
int open_static_file (int root_fd, size_t root_path_length, const char * path,
    static_file_t * file) {
    file->fd = -1;
    int status_code = check_path_components(root_path_length, path);
    if (status_code != 0) {
        return status_code;
    }
    int error_number = 0;
    int target_fd = walk_path(root_fd, path, &error_number);
    if (target_fd < 0) {
        return convert_open_error(error_number);
    }
    return prepare_target(target_fd, path, file);
}

/* ── 도우미 함수 ── */

/* 파일을 열기 전에 숨김 조각과 길이를 먼저 거른다 (STA-03, STA-04) */
static int check_path_components (size_t root_path_length, const char * path) {
    if (root_path_length + strlen(path) >= PATH_MAX) {
        return 414;
    }
    const char * cursor = path;
    while (*cursor != '\0') {
        if (*cursor == '/') {
            cursor++;
            continue;
        }
        size_t length = strcspn(cursor, "/");
        if (cursor[0] == '.') {
            return 404;
        }
        if (length > NAME_MAX) {
            return 414;
        }
        cursor += length;
    }
    return 0;
}

/* 열린 마지막 조각의 fd를 돌려준다. 실패하면 -1과 error_number */
static int walk_path (int root_fd, const char * path, int * error_number) {
    int current_fd = dup(root_fd);
    if (current_fd < 0) {
        *error_number = errno;
        return -1;
    }
    const char * cursor = path;
    while (*cursor != '\0' && current_fd >= 0) {
        if (*cursor == '/') {
            cursor++;
            continue;
        }
        size_t length = strcspn(cursor, "/");
        current_fd = open_next_component(current_fd, cursor, length,
            error_number);
        cursor += length;
    }
    return current_fd;
}

/* directory_fd는 넘겨받아서 성공이든 실패든 이 함수가 닫는다 */
static int open_next_component (int directory_fd, const char * name,
    size_t name_length, int * error_number) {
    char component[NAME_MAX + 1];
    memcpy(component, name, name_length);
    component[name_length] = '\0';
    int next_fd = openat(directory_fd, component, OPEN_FLAGS);
    *error_number = errno;
    close(directory_fd);
    return next_fd;
}

static int convert_open_error (int error_number) {
    if (error_number == ELOOP || error_number == EACCES
        || error_number == EPERM) {
        return 403;
    }
    if (error_number == ENOENT || error_number == ENOTDIR) {
        return 404;
    }
    if (error_number == ENAMETOOLONG) {
        return 414;
    }
    return 500;
}

static int prepare_target (int target_fd, const char * path,
    static_file_t * file) {
    struct stat info;
    if (fstat(target_fd, &info) != 0) {
        close(target_fd);
        return 500;
    }
    if (S_ISDIR(info.st_mode)) {
        return prepare_directory(target_fd, path, file);
    }
    if (!S_ISREG(info.st_mode)) {
        close(target_fd);
        return 403;
    }
    if (has_trailing_slash(path)) {
        close(target_fd);
        return 404;
    }
    return fill_regular_file(target_fd, strrchr(path, '/') + 1, file);
}

/* directory_fd는 넘겨받아서 이 함수가 닫는다 */
static int prepare_directory (int directory_fd, const char * path,
    static_file_t * file) {
    if (!has_trailing_slash(path)) {
        close(directory_fd);
        return 301;
    }
    int index_fd = openat(directory_fd, "index.html", OPEN_FLAGS);
    int error_number = errno;
    close(directory_fd);
    if (index_fd < 0) {
        return convert_index_error(error_number);
    }
    return fill_regular_file(index_fd, "index.html", file);
}

static int has_trailing_slash (const char * path) {
    size_t length = strlen(path);
    return length > 0 && path[length - 1] == '/';
}

/* index.html이 없으면 폴더 목록을 보여 주지 않고 403으로 막는다 */
static int convert_index_error (int error_number) {
    if (error_number == ENOENT || error_number == ENOTDIR) {
        return 403;
    }
    return convert_open_error(error_number);
}

/* 크기는 이미 연 fd에서 재야 확인과 읽기 사이에 파일이 바뀌어도 맞는다 */
static int fill_regular_file (int file_fd, const char * name,
    static_file_t * file) {
    struct stat info;
    if (fstat(file_fd, &info) != 0) {
        close(file_fd);
        return 500;
    }
    if (!S_ISREG(info.st_mode)) {
        close(file_fd);
        return 403;
    }
    file->fd = file_fd;
    file->size = info.st_size;
    file->mime_type = find_mime_type(name);
    return 200;
}
