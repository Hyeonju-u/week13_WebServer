#include "access_log.h"

#include <arpa/inet.h>
#include <stdio.h>
#include <time.h>

/* (L37) "[24/Nov/2026:13:05:01 +0000]" 28글자 + '\0' */
#define LOG_TIME_SIZE 32

static void write_log_time (char * text);
static void write_status (int status, char * text, size_t size);
static void write_address (const struct sockaddr_in * address, char * text);
static void write_backend_part (const access_entry_t * entry);

/* ── 공개 함수 ── */

/* (L15, L37, G18, G39) 프록시 요청이면 줄 끝에 백엔드 부분을 붙인다 */
void write_access_log (const access_entry_t * entry) {
    char time_text[LOG_TIME_SIZE];
    char address[INET_ADDRSTRLEN];
    char status_text[8];
    const char * line = entry->request_line;
    int line_length = (int) entry->request_line_length;

    write_log_time(time_text);
    write_address(entry->peer, address);
    if (line == NULL) {
        line = "-";
        line_length = 1;
    }
    write_status(entry->status, status_text, sizeof(status_text));
    printf("%s %s:%u \"%.*s\" %s %zu %lld%s%s", time_text, address,
           (unsigned) ntohs(entry->peer->sin_port), line_length, line,
           status_text, entry->bytes_sent, entry->elapsed_ms,
           entry->tag == NULL ? "" : " ",
           entry->tag == NULL ? "" : entry->tag);
    write_backend_part(entry);
    printf("\n");
}

/* ── 도우미 함수 ── */

/* (L37) nginx 형식, UTC */
static void write_log_time (char * text) {
    time_t now = time(NULL);
    struct tm parts;

    if (gmtime_r(&now, &parts) == NULL
        || strftime(text, LOG_TIME_SIZE, "[%d/%b/%Y:%H:%M:%S +0000]",
                    &parts) == 0) {
        snprintf(text, LOG_TIME_SIZE, "[-]");
    }
}

/* (L37) 응답 없음은 "-" */
static void write_status (int status, char * text, size_t size) {
    if (status == 0) {
        snprintf(text, size, "-");
        return;
    }
    snprintf(text, size, "%d", status);
}

/* (L37) text: INET_ADDRSTRLEN */
static void write_address (const struct sockaddr_in * address, char * text) {
    if (inet_ntop(AF_INET, &address->sin_addr, text,
                  INET_ADDRSTRLEN) == NULL) {
        snprintf(text, INET_ADDRSTRLEN, "-");
    }
}

/* (G18, G39) " -> IP:포트[ retry=N] (backend Nms)[ 원인]" */
static void write_backend_part (const access_entry_t * entry) {
    char address[INET_ADDRSTRLEN];

    if (entry->backend == NULL) {
        return;
    }
    write_address(entry->backend, address);
    printf(" -> %s:%u", address, (unsigned) ntohs(entry->backend->sin_port));
    if (entry->retry_count > 0) {
        printf(" retry=%zu", entry->retry_count);
    }
    printf(" (backend %lldms)", entry->backend_ms);
    if (entry->backend_cause != NULL) {
        printf(" %s", entry->backend_cause);
    }
}
