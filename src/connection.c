#include "connection.h"

#include <stdlib.h>
#include <unistd.h>

/* 가장 큰 메모리는 필요할 때 처음 쓰이는 버퍼가 가져가므로 구조체는 가볍다 */
conn_t * create_connection (int fd, conn_kind_t kind) {
    conn_t * conn = calloc(1, sizeof(*conn));
    if (conn == NULL) {
        return NULL;
    }
    conn->fd = fd;
    conn->kind = kind;
    conn->file_fd = -1;
    conn->heap_index = -1;
    init_buffer(&conn->input);
    init_buffer(&conn->output);
    init_buffer(&conn->forward);
    return conn;
}

/* 아직 열려 있는 fd와 버퍼도 함께 정리해서 정리 경로가 하나로 모인다 */
void destroy_connection (conn_t * conn) {
    if (conn->fd >= 0) {
        close(conn->fd);
    }
    if (conn->file_fd >= 0) {
        close(conn->file_fd);
    }
    release_buffer(&conn->input);
    release_buffer(&conn->output);
    release_buffer(&conn->forward);
    free(conn);
}
