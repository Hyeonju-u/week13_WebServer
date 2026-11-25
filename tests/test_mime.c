#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "mime.h"

static void expect_type (const char * name, const char * expected) {
    assert(strcmp(find_mime_type(name), expected) == 0);
}

int main (void) {
    expect_type("index.html", "text/html; charset=utf-8");
    expect_type("a.htm", "text/html; charset=utf-8");
    expect_type("style.css", "text/css; charset=utf-8");
    expect_type("app.js", "text/javascript; charset=utf-8");
    expect_type("CAT.PNG", "image/png");
    expect_type("photo.jpg", "image/jpeg");
    expect_type("photo.JPEG", "image/jpeg");
    expect_type("archive.tar.gz", "application/octet-stream");
    expect_type("README", "application/octet-stream");
    expect_type("trailing.", "application/octet-stream");
    expect_type("evil.html.txt", "application/octet-stream");
    printf("test_mime: 모두 통과\n");
    return 0;
}
