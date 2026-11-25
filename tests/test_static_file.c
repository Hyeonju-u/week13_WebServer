#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "static_file.h"

static char root_path[256];
static int root_fd = -1;

static void write_file (const char * relative, const char * content) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", root_path, relative);
    FILE * file = fopen(path, "w");
    assert(file != NULL);
    fputs(content, file);
    fclose(file);
}

static void make_directory (const char * relative) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", root_path, relative);
    assert(mkdir(path, 0755) == 0);
}

static void make_link (const char * target, const char * relative) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", root_path, relative);
    assert(symlink(target, path) == 0);
}

static void prepare_tree (void) {
    snprintf(root_path, sizeof(root_path), "/tmp/ws_static_XXXXXX");
    assert(mkdtemp(root_path) != NULL);
    write_file("index.html", "<html>root</html>");
    write_file("a.txt", "text");
    write_file(".secret", "hidden");
    make_directory("sub");
    write_file("sub/index.html", "sub");
    make_directory("empty");
    make_link("/etc/hosts", "outside_link");
    make_link("sub", "dir_link");
    make_link("index.html", "inside_link");
    char fifo_path[512];
    snprintf(fifo_path, sizeof(fifo_path), "%s/fifo", root_path);
    assert(mkfifo(fifo_path, 0644) == 0);
    root_fd = open(root_path, O_RDONLY);
    assert(root_fd >= 0);
}

static int open_path (const char * path, static_file_t * file) {
    return open_static_file(root_fd, strlen(root_path), path, file);
}

static void expect_status (const char * path, int expected) {
    static_file_t file;
    int status_code = open_path(path, &file);
    if (status_code != expected) {
        fprintf(stderr, "%s: 기대 %d, 실제 %d\n", path, expected, status_code);
        abort();
    }
    if (status_code == 200) {
        close(file.fd);
    } else {
        assert(file.fd == -1);
    }
}

static void test_statuses (void) {
    expect_status("/", 200);
    expect_status("/index.html", 200);
    expect_status("/a.txt", 200);
    expect_status("//a.txt", 200);
    expect_status("/sub", 301);
    expect_status("/sub/", 200);
    expect_status("/empty/", 403);
    expect_status("/empty", 301);
    expect_status("/nope", 404);
    expect_status("/nope/x", 404);
    expect_status("/a.txt/", 404);
    expect_status("/a.txt/x", 404);
}

static void test_blocked_paths (void) {
    expect_status("/.secret", 404);
    expect_status("/.git/config", 404);
    expect_status("/sub/../a.txt", 404);
    expect_status("/../etc/passwd", 404);
    expect_status("/outside_link", 403);
    expect_status("/inside_link", 403);
    expect_status("/dir_link/index.html", 403);
    expect_status("/dir_link/", 403);
    expect_status("/fifo", 403);
}

static void test_length_limits (void) {
    char long_name[400];
    memset(long_name, 'a', sizeof(long_name));
    long_name[0] = '/';
    long_name[sizeof(long_name) - 1] = '\0';
    expect_status(long_name, 414);
    char long_path[5000];
    memset(long_path, 'a', sizeof(long_path));
    for (size_t i = 0; i < sizeof(long_path); i += 100) {
        long_path[i] = '/';
    }
    long_path[sizeof(long_path) - 1] = '\0';
    expect_status(long_path, 414);
}

static void test_file_details (void) {
    static_file_t file;
    assert(open_path("/index.html", &file) == 200);
    assert(file.size == 17);
    assert(strcmp(file.mime_type, "text/html; charset=utf-8") == 0);
    close(file.fd);
    assert(open_path("/sub/", &file) == 200);
    assert(file.size == 3);
    close(file.fd);
    assert(open_path("/a.txt", &file) == 200);
    assert(strcmp(file.mime_type, "application/octet-stream") == 0);
    close(file.fd);
}

int main (void) {
    prepare_tree();
    test_statuses();
    test_blocked_paths();
    test_length_limits();
    test_file_details();
    char command[300];
    snprintf(command, sizeof(command), "rm -rf %s", root_path);
    close(root_fd);
    assert(system(command) == 0);
    printf("test_static_file: 모두 통과\n");
    return 0;
}
