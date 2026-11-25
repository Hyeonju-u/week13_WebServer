#ifndef HTTP_REQUEST_H
#define HTTP_REQUEST_H

#include <stdbool.h>
#include <stddef.h>

/* (B4, P28) 경계값까지 허용한다. 본문 상한은 설정 max_body로 바꿈 (G23) */
#define MAX_HEADER_SIZE 8192
#define MAX_BODY_SIZE 1048576

/* (P19) */
typedef enum {
    FIND_FOUND,
    FIND_NEED_MORE,
    FIND_BAD
} find_result;

typedef enum {
    PARSE_OK,
    PARSE_BAD
} parse_result;

/* (P15) */
typedef enum {
    LINE_OK,
    LINE_END,
    LINE_BAD
} line_result;

/* (P26) */
typedef enum {
    BODY_DONE,
    BODY_NEED_MORE,
    BODY_NO_MEMORY
} body_result;

/* (P21) */
typedef enum {
    METHOD_GET,
    METHOD_POST
} method_t;

/* (P20) 헤더 복사본 안을 가리킨다. 끝에 '\0'이 없으니 길이와 같이 쓴다 */
typedef struct {
    const char * name;
    size_t name_length;
    const char * value;
    size_t value_length;
} header_view_t;

/* (P21, P24) 연결 구조체에 박아 두고 0으로 채운 채로 시작한다.
 * head·body만 소유하고, 나머지 포인터는 head 안을 가리킨다 */
typedef struct {
    char * head;
    size_t head_length;
    method_t method;
    const char * path;
    size_t path_length;
    const char * host;
    size_t host_length;
    const char * version;
    size_t version_length;
    /* (P22) 요청 라인 다음부터 마지막 빈 줄 앞까지 */
    const char * headers;
    size_t headers_length;
    size_t content_length;
    bool keep_alive;
    int status;
    char * body;
    size_t body_received;
    /* (P10) GET 본문을 할당 없이 버릴 때 남은 바이트 수 */
    size_t discard_remaining;
} request_t;

/* ── 공개 함수 ── */

/* (P3, P17) 맨 앞의 \r\n을 모두 지우고 남은 바이트를 앞으로 당긴다.
 * 반환: false = 맨 앞이 \r 없는 \n (400, B29) */
bool skip_leading_empty_lines (char * buffer, size_t * length);

/* (R1c, P16, P29) 앞 MAX_HEADER_SIZE 바이트만 본다.
 * FIND_FOUND: *header_length = 마지막 빈 줄까지 포함한 바이트 수
 * FIND_BAD: *status = 400 / 414 / 431 */
find_result find_header_end (const char * buffer, size_t length,
                             size_t * header_length, int * status);

/* (P13, P14, P25) 반환: false = 할당 실패(500). 이때 request는 그대로 */
bool copy_request_head (request_t * request, const char * buffer,
                        size_t header_length);

/* (R1b, R1c) head: 보통 request->head (빌려 씀)
 * PARSE_BAD: request->status만 쓴다 (400, 411, 413, 501, 505) */
parse_result parse_request_head (const char * head, size_t head_length,
                                 request_t * request);

/* (G9, G23) parse_request_head와 같고, 본문 상한(413)만 max_body로 본다 */
parse_result parse_request_head_limited (const char * head,
                                         size_t head_length, size_t max_body,
                                         request_t * request);

/* (P5, P13, P15) block: 헤더 줄 영역 (P22).
 * LINE_OK면 *position을 다음 줄 시작으로 옮긴다 */
line_result next_header_line (const char * block, size_t length,
                              size_t * position, header_view_t * line);

/* (R2a, P10, P11, P26) 연결 버퍼에 남은 본문 앞부분을 본문 버퍼로 옮기거나
 * (POST) 버리고(GET) 그만큼 당긴다. Content-Length를 넘는 바이트는 남긴다 */
body_result start_body (request_t * request, char * buffer,
                        size_t * buffer_length);

/* (L20) 연결 버퍼에 남은 본문 앞부분을 메서드와 관계없이 버리고 당긴다.
 * 이후 읽기는 discard_remaining만큼만 하고 add_body_bytes로 알린다 */
body_result start_discard_body (request_t * request, char * buffer,
                                size_t * buffer_length);

/* (G33) start_body와 같지만 메서드와 관계없이 모은다 (프록시 경로) */
body_result start_collect_body (request_t * request, char * buffer,
                                size_t * buffer_length);

/* (R3c, P26, L25) 소켓이 body + body_received 자리에 (버리는 중이면
 * 버릴 곳에) 남은 양 이하의 count바이트를 읽은 뒤 부른다 */
body_result add_body_bytes (request_t * request, size_t count);

/* (P27) */
void remove_buffer_prefix (char * buffer, size_t * length, size_t count);

/* (P24) 두 번 불러도 안전하다. 해제한 포인터는 NULL로 만든다 */
void destroy_request (request_t * request);

#endif
