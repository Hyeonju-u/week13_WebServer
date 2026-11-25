#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "proxy_config.h"

static void test_parse_and_match (void) {
    proxy_config_t * config = create_proxy_config_from_text(
        "route /api 127.0.0.1:8001 127.0.0.1:8002 127.0.0.1:8003\n"
        "route /api/admin 127.0.0.1:8004\n"
        "\n"
        "route /static/ 127.0.0.1:8005\n"
        "connect_timeout 2\n"
        "response_timeout 7\n");
    assert(config != NULL);
    assert(config->route_count == 3);
    assert(config->connect_timeout_seconds == 2);
    assert(config->response_timeout_seconds == 7);
    route_t * route = find_route(config, "/api");
    assert(route != NULL && route->backend_count == 3);
    assert(find_route(config, "/api/") == route);
    assert(find_route(config, "/api/users") == route);
    assert(find_route(config, "/apix") == NULL);
    assert(find_route(config, "/api2/users") == NULL);
    route_t * admin = find_route(config, "/api/admin/x");
    assert(admin != NULL && admin->backend_count == 1);
    assert(find_route(config, "/api/administrator") == route);
    assert(find_route(config, "/static/a.css") != NULL);
    assert(find_route(config, "/index.html") == NULL);
    assert(find_route(config, "/") == NULL);
    destroy_proxy_config(config);
}

static void test_defaults (void) {
    proxy_config_t * config = create_proxy_config_from_text("");
    assert(config != NULL);
    assert(config->connect_timeout_seconds == 3);
    assert(config->response_timeout_seconds == 10);
    assert(find_route(config, "/anything") == NULL);
    destroy_proxy_config(config);
}

static void test_invalid_configs (void) {
    assert(create_proxy_config_from_text("route /api\n") == NULL);
    assert(create_proxy_config_from_text("route api 127.0.0.1:80\n") == NULL);
    assert(create_proxy_config_from_text("route /api 127.0.0.1\n") == NULL);
    assert(create_proxy_config_from_text("route /api 127.0.0.1:abc\n")
        == NULL);
    assert(create_proxy_config_from_text("bogus 1\n") == NULL);
    assert(create_proxy_config_from_text("connect_timeout abc\n") == NULL);
    assert(create_proxy_config_from_text("response_timeout 0\n") == NULL);
    assert(create_proxy_config_from_text("response_timeout 5 6\n") == NULL);
}

int main (void) {
    test_parse_and_match();
    test_defaults();
    test_invalid_configs();
    printf("test_route: 모두 통과\n");
    return 0;
}
