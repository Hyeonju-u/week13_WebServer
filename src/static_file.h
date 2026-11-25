#ifndef STATIC_FILE_H
#define STATIC_FILE_H

#include <stddef.h>
#include <sys/types.h>

typedef struct {
    int fd;
    off_t size;
    const char * mime_type;
} static_file_t;

int open_static_file (int root_fd, size_t root_path_length, const char * path,
    static_file_t * file);

#endif
