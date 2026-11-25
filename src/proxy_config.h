#ifndef PROXY_CONFIG_H
#define PROXY_CONFIG_H

#include <stddef.h>
#include <sys/socket.h>

typedef struct {
    struct sockaddr_storage address;
    socklen_t address_length;
} backend_t;

typedef struct {
    char * prefix;
    size_t prefix_length;
    backend_t * backends;
    size_t backend_count;
} route_t;

typedef struct {
    route_t * routes;
    size_t route_count;
    int connect_timeout_seconds;
    int response_timeout_seconds;
    int backend_fail_timeout_seconds;
} proxy_config_t;

proxy_config_t * create_empty_proxy_config (void);
proxy_config_t * create_proxy_config (const char * file_path);
proxy_config_t * create_proxy_config_from_text (const char * text);
void destroy_proxy_config (proxy_config_t * config);
route_t * find_route (proxy_config_t * config, const char * path);

#endif
