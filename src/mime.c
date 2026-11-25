#include "mime.h"

#include <string.h>
#include <strings.h>

typedef struct {
    const char * extension;
    const char * mime_type;
} mime_entry_t;

/* 새 타입은 이 표에 한 줄만 더하면 된다 (STA-15) */
static const mime_entry_t mime_table[] = {
    { "html", "text/html; charset=utf-8" },
    { "htm", "text/html; charset=utf-8" },
    { "css", "text/css; charset=utf-8" },
    { "js", "text/javascript; charset=utf-8" },
    { "png", "image/png" },
    { "jpg", "image/jpeg" },
    { "jpeg", "image/jpeg" },
};

#define UNKNOWN_MIME_TYPE "application/octet-stream"

/* ── 공개 함수 ── */

/* 모르는 확장자를 html로 보내면 올려 둔 파일이 실행되므로 octet-stream으로 둔다 */
const char * find_mime_type (const char * file_name) {
    const char * dot = strrchr(file_name, '.');
    if (dot == NULL) {
        return UNKNOWN_MIME_TYPE;
    }
    size_t count = sizeof(mime_table) / sizeof(mime_table[0]);
    for (size_t i = 0; i < count; i++) {
        if (strcasecmp(dot + 1, mime_table[i].extension) == 0) {
            return mime_table[i].mime_type;
        }
    }
    return UNKNOWN_MIME_TYPE;
}
