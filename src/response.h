#ifndef RESPONSE_H
#define RESPONSE_H

#include "http.h"
#include <stdint.h>

typedef struct {
    char header[512];
    size_t header_length;
    size_t header_sent;
    int file;
    char chunk[8192];
    const char * body;
    size_t pending;
    uintmax_t remaining;
} response_t;

void build_local_response(response_t * response, const request_t * request,
                          const char * body, int root);
void prepare_error(response_t * response, int status);
void prepare_proxy_response(response_t * response, const char * status,
                            const char * type, const char * body, size_t size);
int send_response(response_t * response, int client);

#endif
