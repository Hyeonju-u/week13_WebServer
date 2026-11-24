#include "http.h"
#include "proxy.h"
#include "response.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#define MAX_CLIENTS 128

typedef struct {
    int socket_fd;
    char headers[HEADER_LIMIT];
    size_t header_read;
    bool headers_complete;
    request_t request;
    char * body;
    size_t body_read;
    response_t response;
    proxy_t * proxy;
} client_t;

typedef struct {
    int epoll_fd;
    int listener;
    proxy_route_t route;
    int free_slots[MAX_CLIENTS];
    int free_count;
    client_t * clients[MAX_CLIENTS + 1];
} server_t;

static int open_listen_socket(const char * host, const char * port);
static int set_nonblocking(int socket_fd);
static int serve_clients(int listener, int root, proxy_route_t route);
static int run_event_loop(server_t * server, int root);
static int set_socket_events(int epoll_fd, int operation, int socket_fd,
                             int slot, uint32_t events);
static void accept_client(server_t * server);
static void handle_client_event(server_t * server, struct epoll_event event,
                                int root);
static void read_request_and_prepare_response(server_t * server, int slot,
                                              int root);
static void start_proxy_request(server_t * server, int slot, const char * path);
static void handle_backend_event(server_t * server, int slot);
static void finish_proxy_request(server_t * server, int slot, int status);
static int check_proxy_timeouts(server_t * server);
static int read_request(client_t * client, int socket_fd);
static ssize_t receive_bytes(int socket_fd, char * buffer, size_t length);
static int read_request_headers(client_t * client, int socket_fd);
static int classify_full_header(const char * headers);
static void close_client(server_t * server, int slot);

/* ── 공개 함수 ── */

int main (int argc, char ** argv) {
    proxy_route_t route = {0};
    if (argc != 3 && argc != 4 && argc != 7) {
        fprintf(stderr, "Usage: %s PORT ROOT [LISTEN_IPV4 "
                "[PREFIX BACKEND_IPV4 BACKEND_PORT]]\n", argv[0]);
        return EXIT_FAILURE;
    }
    if (argc == 7 && !set_proxy_route(&route, argv[4], argv[5], argv[6])) {
        fprintf(stderr, "Invalid reverse proxy route\n");
        return EXIT_FAILURE;
    }
    const char * host = argc >= 4 ? argv[3] : "0.0.0.0";
    int root = open(argv[2], O_RDONLY | O_DIRECTORY);
    if (root < 0) {
        perror("static root");
        return EXIT_FAILURE;
    }
    int listener = open_listen_socket(host, argv[1]);
    if (listener < 0) {
        perror("listen");
        close(root);
        return EXIT_FAILURE;
    }
    /* 응답 도중 탭을 닫아도 다른 연결은 계속 서비스한다. */
    signal(SIGPIPE, SIG_IGN);
    int result = serve_clients(listener, root, route);
    close(listener);
    close(root);
    return result;
}

/* ── 도우미 함수 ── */

static int open_listen_socket (const char * host, const char * port) {
    struct sockaddr_in address = {.sin_family = AF_INET};
    unsigned long number = strtoul(port, NULL, 10);
    if (*port == '\0' || port[strspn(port, "0123456789")] != '\0'
        || number > 65535 || inet_pton(AF_INET, host, &address.sin_addr) != 1) {
        errno = EINVAL;
        return -1;
    }
    address.sin_port = htons((unsigned short)number);
    int listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0) {
        return -1;
    }
    int enabled = 1;
    if (setsockopt(listener, SOL_SOCKET, SO_REUSEADDR,
                   &enabled, sizeof(enabled)) < 0
        || set_nonblocking(listener) < 0
        || bind(listener, (struct sockaddr *)&address, sizeof(address)) < 0
        || listen(listener, MAX_CLIENTS) < 0) {
        close(listener);
        return -1;
    }
    socklen_t length = sizeof(address);
    if (getsockname(listener, (struct sockaddr *)&address, &length) < 0) {
        close(listener);
        return -1;
    }
    printf("Listening on %s:%u\n", host, (unsigned)ntohs(address.sin_port));
    fflush(stdout);
    return listener;
}

static int set_nonblocking (int socket_fd) {
    int flags = fcntl(socket_fd, F_GETFL);
    if (flags < 0) {
        return -1;
    }
    return fcntl(socket_fd, F_SETFL, flags | O_NONBLOCK);
}

static int serve_clients (int listener, int root, proxy_route_t route) {
    server_t server = {.listener = listener, .route = route,
                       .free_count = MAX_CLIENTS};
    server.epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (server.epoll_fd < 0) {
        perror("epoll_create1");
        return EXIT_FAILURE;
    }
    for (int i = 0; i < MAX_CLIENTS; i++) {
        server.free_slots[i] = i + 1;
    }
    int result = run_event_loop(&server, root);
    for (int i = 1; i <= MAX_CLIENTS; i++) {
        if (server.clients[i] != NULL) {
            close_client(&server, i);
        }
    }
    close(server.epoll_fd);
    return result;
}

static int run_event_loop (server_t * server, int root) {
    struct epoll_event events[MAX_CLIENTS + 1];
    bool accepting = false;
    for (;;) {
        int timeout = check_proxy_timeouts(server);
        /* 슬롯이 가득 차면 대기 중인 새 연결이 루프를 계속 깨우지 않게 한다. */
        bool has_space = server->free_count > 0;
        if (accepting != has_space) {
            int operation = has_space ? EPOLL_CTL_ADD : EPOLL_CTL_DEL;
            if (set_socket_events(server->epoll_fd, operation, server->listener,
                                  0, EPOLLIN) < 0) {
                return EXIT_FAILURE;
            }
            accepting = has_space;
        }
        int ready = epoll_wait(server->epoll_fd, events, MAX_CLIENTS + 1,
                               timeout);
        if (ready < 0 && errno == EINTR) {
            continue;
        }
        if (ready < 0) {
            perror("epoll_wait");
            return EXIT_FAILURE;
        }
        for (int i = 0; i < ready; i++) {
            if (events[i].data.u32 != 0) {
                handle_client_event(server, events[i], root);
                continue;
            }
            if (events[i].events & (EPOLLERR | EPOLLHUP)) {
                fprintf(stderr, "listener disconnected\n");
                return EXIT_FAILURE;
            }
            accept_client(server);
        }
    }
}

static int set_socket_events (int epoll_fd, int operation, int socket_fd,
                              int slot, uint32_t events) {
    struct epoll_event event = {.events = events, .data.u32 = (uint32_t)slot};
    int result = epoll_ctl(epoll_fd, operation, socket_fd, &event);
    if (result < 0) {
        perror("epoll_ctl");
    }
    return result;
}

static void accept_client (server_t * server) {
    int socket_fd = accept(server->listener, NULL, NULL);
    if (socket_fd < 0) {
        return;
    }
    if (set_nonblocking(socket_fd) < 0) {
        close(socket_fd);
        return;
    }
    client_t * client = calloc(1, sizeof(*client));
    if (client == NULL) {
        close(socket_fd);
        return;
    }
    int slot = server->free_slots[server->free_count - 1];
    if (set_socket_events(server->epoll_fd, EPOLL_CTL_ADD, socket_fd, slot,
                          EPOLLIN) < 0) {
        close(socket_fd);
        free(client);
        return;
    }
    client->socket_fd = socket_fd;
    client->response.file = -1;
    server->clients[slot] = client;
    server->free_count--;
}

static void handle_client_event (server_t * server, struct epoll_event event,
                                 int root) {
    int slot = (int)event.data.u32;
    client_t * client = server->clients[slot];
    if (client->proxy != NULL && client->proxy->socket_fd >= 0) {
        handle_backend_event(server, slot);
        return;
    }
    if (event.events & EPOLLERR) {
        close_client(server, slot);
        return;
    }
    if (event.events & EPOLLIN) {
        read_request_and_prepare_response(server, slot, root);
        return;
    }
    if (event.events & EPOLLOUT) {
        int result = send_response(&client->response, client->socket_fd);
        if (result == 0) {
            return;
        }
        if (result > 0) {
            /* 요청이 더 남아 있어도 완성된 오류 응답의 끝을 먼저 알린다. */
            shutdown(client->socket_fd, SHUT_WR);
        }
    }
    close_client(server, slot);
}

static void read_request_and_prepare_response (server_t * server, int slot,
                                               int root) {
    client_t * client = server->clients[slot];
    int status = read_request(client, client->socket_fd);
    if (status < 0) {
        close_client(server, slot);
        return;
    }
    if (status == 0) {
        return;
    }
    if (status == 200) {
        const char * path = find_backend_path(&server->route,
                                              client->request.path);
        if (path != NULL && (strcmp(client->request.method, "GET") == 0
                             || strcmp(client->request.method, "POST") == 0)) {
            start_proxy_request(server, slot, path);
            return;
        }
        build_local_response(&client->response, &client->request, client->body,
                             root);
    } else {
        prepare_error(&client->response, status);
    }
    if (set_socket_events(server->epoll_fd, EPOLL_CTL_MOD, client->socket_fd,
                          slot, EPOLLOUT) < 0) {
        close_client(server, slot);
    }
}

/* 같은 슬롯에서 백엔드와 클라이언트 중 하나의 fd만 감시한다. */
static void start_proxy_request (server_t * server, int slot,
                                 const char * path) {
    client_t * client = server->clients[slot];
    if (set_socket_events(server->epoll_fd, EPOLL_CTL_DEL, client->socket_fd,
                          slot, 0) < 0) {
        close_client(server, slot);
        return;
    }
    client->proxy = create_backend_connection(&server->route, path,
                                              &client->request);
    if (client->proxy == NULL
        || set_socket_events(server->epoll_fd, EPOLL_CTL_ADD,
                             client->proxy->socket_fd, slot, EPOLLOUT) < 0) {
        finish_proxy_request(server, slot, 502);
    }
}

static void handle_backend_event (server_t * server, int slot) {
    client_t * client = server->clients[slot];
    proxy_t * proxy = client->proxy;
    bool was_reading = proxy->reading;
    int status = send_request_or_read_response(proxy, &client->request,
                                               client->body, &client->response);
    if (status != 0) {
        finish_proxy_request(server, slot, status);
        return;
    }
    if (!was_reading && proxy->reading
        && set_socket_events(server->epoll_fd, EPOLL_CTL_MOD, proxy->socket_fd,
                             slot, EPOLLIN) < 0) {
        finish_proxy_request(server, slot, 502);
    }
}

static void finish_proxy_request (server_t * server, int slot, int status) {
    client_t * client = server->clients[slot];
    if (client->proxy != NULL) {
        close(client->proxy->socket_fd);
        client->proxy->socket_fd = -1;
    }
    if (status != 1) {
        prepare_error(&client->response, status);
    }
    if (set_socket_events(server->epoll_fd, EPOLL_CTL_ADD, client->socket_fd,
                          slot, EPOLLOUT) < 0) {
        close_client(server, slot);
    }
}

/* 소켓 이벤트가 없어도 가장 이른 백엔드 마감 시각에 깨어난다. */
static int check_proxy_timeouts (server_t * server) {
    if (server->route.prefix == NULL) {
        return -1;
    }
    int timeout = -1;
    int64_t now = monotonic_ms();
    for (int slot = 1; slot <= MAX_CLIENTS; slot++) {
        client_t * client = server->clients[slot];
        if (client == NULL || client->proxy == NULL
            || client->proxy->socket_fd < 0) {
            continue;
        }
        int64_t left = client->proxy->deadline - now;
        if (left <= 0) {
            finish_proxy_request(server, slot, 504);
        } else if (timeout < 0 || left < timeout) {
            timeout = (int)left;
        }
    }
    return timeout;
}

/* 0은 추가 수신, -1은 연결 종료, 양수는 응답할 HTTP 상태 코드다. */
static int read_request (client_t * client, int socket_fd) {
    if (!client->headers_complete) {
        return read_request_headers(client, socket_fd);
    }
    size_t remaining = client->request.body_length - client->body_read;
    ssize_t count = receive_bytes(socket_fd, client->body + client->body_read,
                                  remaining);
    if (count == 0) {
        return 400;
    }
    if (count < 0) {
        return count == -2 ? 0 : -1;
    }
    client->body_read += (size_t)count;
    return client->body_read == client->request.body_length ? 200 : 0;
}

/* -2는 논블로킹 재시도, -1은 복구할 수 없는 소켓 오류다. */
static ssize_t receive_bytes (int socket_fd, char * buffer, size_t length) {
    ssize_t count = recv(socket_fd, buffer, length, 0);
    if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK
                      || errno == EINTR)) {
        return -2;
    }
    return count;
}

static int read_request_headers (client_t * client, int socket_fd) {
    size_t previous = client->header_read;
    ssize_t count = receive_bytes(socket_fd, client->headers + previous,
                                  HEADER_LIMIT - previous);
    if (count == 0) {
        return previous == 0 ? -1 : 400;
    }
    if (count < 0) {
        return count == -2 ? 0 : -1;
    }
    client->header_read += (size_t)count;
    /* 직전 수신의 끝에서 시작한 CRLF 경계도 이번 검색에 포함한다. */
    size_t start = previous > 3 ? previous - 3 : 0;
    size_t end = find_header_end(client->headers, client->header_read, start);
    if (end == 0) {
        if (client->header_read == HEADER_LIMIT) {
            return classify_full_header(client->headers);
        }
        return 0;
    }
    int status = parse_request_headers(client->headers, end, &client->request);
    if (status != 0 || client->request.body_length == 0) {
        return status == 0 ? 200 : status;
    }
    client->body = malloc(client->request.body_length);
    if (client->body == NULL) {
        return 500;
    }
    size_t received = client->header_read - end;
    if (received > client->request.body_length) {
        received = client->request.body_length;
    }
    memcpy(client->body, client->headers + end, received);
    client->body_read = received;
    client->headers_complete = true;
    return received == client->request.body_length ? 200 : 0;
}

/* 헤더 버퍼가 먼저 차도 긴 요청 대상에는 414를 반환한다. */
static int classify_full_header (const char * headers) {
    const char * space = memchr(headers, ' ', HEADER_LIMIT);
    if (space == NULL) {
        return 431;
    }
    const char * target = space + 1;
    size_t available = (size_t)(headers + HEADER_LIMIT - target);
    const char * end = memchr(target, ' ', available);
    size_t length = end == NULL ? available : (size_t)(end - target);
    return length > TARGET_LIMIT ? 414 : 431;
}

static void close_client (server_t * server, int slot) {
    client_t * client = server->clients[slot];
    if (client->response.file >= 0) {
        close(client->response.file);
    }
    /* 연결 소켓의 마지막 fd를 닫으면 epoll 등록도 해제된다. */
    close(client->socket_fd);
    destroy_backend_connection(client->proxy);
    free(client->body);
    free(client);
    server->clients[slot] = NULL;
    server->free_slots[server->free_count++] = slot;
}
