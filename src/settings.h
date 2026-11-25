#ifndef SETTINGS_H
#define SETTINGS_H

/* 기본 설정값 (정의서 12장). 값을 바꾸면 README의 표도 함께 고친다. */
#define REQUEST_LINE_MAX 8192
#define HEADER_BYTES_MAX 16384
#define HEADER_COUNT_MAX 100
#define BODY_BYTES_MAX (1024 * 1024)
#define BUFFER_INITIAL_BYTES 4096
/* 테스트에서 작은 값으로 다시 빌드해 503 경로를 확인할 수 있게 덮어쓸 수 있다 */
#ifndef BUFFER_TOTAL_MAX
#define BUFFER_TOTAL_MAX ((size_t) 512 * 1024 * 1024)
#endif
#define REQUEST_TIMEOUT_MS 15000
#define KEEP_ALIVE_TIMEOUT_MS 5000
#define SHUTDOWN_WAIT_MS 5000
#define DEFAULT_MAX_CONNECTIONS 10000
#define DEFAULT_PORT 8080
#define FILE_CHUNK_BYTES 65536
#define BACKPRESSURE_BYTES 65536
#define ACCEPT_BATCH_MAX 64
#define RESPONSE_HEADER_MAX 16384
#define DEFAULT_CONNECT_TIMEOUT_SECONDS 3
#define DEFAULT_RESPONSE_TIMEOUT_SECONDS 10
/* 연결·응답 타임아웃이 난 백엔드를 Round-Robin 후보에서 빼 두는 시간 (#2 G13) */
#define DEFAULT_BACKEND_FAIL_TIMEOUT_SECONDS 10

/* 요청 하나가 차지할 수 있는 최대 크기: 요청 라인 + 헤더 + 본문 + 줄바꿈 여유 */
#define INPUT_BUFFER_MAX \
    (REQUEST_LINE_MAX + HEADER_BYTES_MAX + BODY_BYTES_MAX + 16)

/* 응답을 보내는 동안 다음 요청을 미리 받아 둘 수 있는 양 */
#define PIPELINE_BYTES_MAX 16384

#endif
