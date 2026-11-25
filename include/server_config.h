#ifndef SERVER_CONFIG_H
#define SERVER_CONFIG_H

#include <netinet/in.h>
#include <stdbool.h>
#include <stddef.h>

/* (G31) "파일:줄: 이유" 한 줄. 경로가 아주 길면 뒤가 잘린다 */
#define CONFIG_ERROR_SIZE 512

/* (G14) */
typedef enum {
    BALANCE_ROUND_ROBIN,
    BALANCE_LEAST_CONN
} balance_t;

/* (G1, G2, G14) 설정 파일의 route 한 줄 */
typedef struct {
    /* 소유. '\0'으로 끝나고 끝 '/'는 정리됨 (G15, "/"만 예외) */
    char * prefix;
    size_t prefix_length;
    balance_t balance;
    /* 소유. server_config_t.backends 안의 번호, 적힌 순서 그대로 */
    size_t * backend_indexes;
    size_t backend_count;
} route_t;

/* (G15, G29) 시작할 때 한 번 읽고 바뀌지 않는다.
 * 실행 중 바뀌는 값(처리 중 수 등)은 upstream_t에 따로 둔다 */
typedef struct {
    int port;
    /* 소유. copy_static_root 결과 */
    char * root;
    bool access_log;
    /* (L36, G41) 테스트 전용: 카드 수·처리 중 수를 stderr에 */
    bool count_log;
    long long header_timeout_ms;
    long long body_timeout_ms;
    long long send_timeout_ms;
    long long keepalive_timeout_ms;
    long long linger_quiet_ms;
    long long linger_total_ms;
    long long backend_connect_timeout_ms;
    long long backend_send_timeout_ms;
    long long backend_read_timeout_ms;
    long long backend_fail_timeout_ms;
    size_t max_body;
    size_t max_backend_response;
    /* 소유. 적힌 순서 그대로 (G2 먼저 적힌 규칙) */
    route_t * routes;
    size_t route_count;
    /* 소유. 주소마다 하나 (G29: 규칙이 달라도 같은 주소는 같은 백엔드) */
    struct sockaddr_in * backends;
    size_t backend_count;
} server_config_t;

/* (G31) 종료 코드: INVALID = 2 (설정 내용), UNAVAILABLE = 1 (환경) */
typedef enum {
    CONFIG_OK,
    CONFIG_INVALID,
    CONFIG_UNAVAILABLE
} config_result_t;

/* ── 공개 함수 ── */

/* (G15, G31, G40) error: error_size 바이트에 이유 한 줄 ('\0'으로 끝남).
 * CONFIG_OK일 때만 config를 채우고, 실패하면 config는 건드리지 않는다.
 * 성공하면 destroy_server_config로 푼다 */
config_result_t load_server_config (const char * path,
                                    server_config_t * config, char * error,
                                    size_t error_size);

/* (G15) 두 번 불러도 안전하다 (전부 0으로 돌린다) */
void destroy_server_config (server_config_t * config);

#endif
