#include "access_log.h"

#include <arpa/inet.h>
#include <stdio.h>
#include <time.h>

/* (L37) "[24/Nov/2026:13:05:01 +0000]" 28글자 + '\0' */
#define LOG_TIME_SIZE 32

static void write_log_time (char * text);
static void write_status (int status, char * text, size_t size);

/* ── 공개 함수 ── */

/* (L15, L37) */
void write_access_log (const access_entry_t * entry) {
    char time_text[LOG_TIME_SIZE];
    char address[INET_ADDRSTRLEN];
    char status_text[8];
    const char * line = entry->request_line;
    int line_length = (int) entry->request_line_length;

    write_log_time(time_text);
    if (inet_ntop(AF_INET, &entry->peer->sin_addr, address,
                  sizeof(address)) == NULL) {
        snprintf(address, sizeof(address), "-");
    }
    if (line == NULL) {
        line = "-";
        line_length = 1;
    }
    write_status(entry->status, status_text, sizeof(status_text));
    printf("%s %s:%u \"%.*s\" %s %zu %lld%s%s\n", time_text, address,
           (unsigned) ntohs(entry->peer->sin_port), line_length, line,
           status_text, entry->bytes_sent, entry->elapsed_ms,
           entry->tag == NULL ? "" : " ",
           entry->tag == NULL ? "" : entry->tag);
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
