# (B13) make test: 일반 빌드 / make asan: ASan·LSan 빌드 (Q5)
CC = gcc
CFLAGS = -Wall -Wextra -std=c11 -g -Iinclude
ASAN_FLAGS = -fsanitize=address,leak -fno-omit-frame-pointer

SOURCES = src/http_request.c
TEST_SOURCES = tests/test_http_request.c
HEADERS = include/http_request.h tests/check.h

test: build/test_http_request
	./build/test_http_request

asan: build/test_http_request_asan
	./build/test_http_request_asan

build/test_http_request: $(SOURCES) $(TEST_SOURCES) $(HEADERS)
	mkdir -p build
	$(CC) $(CFLAGS) -o $@ $(SOURCES) $(TEST_SOURCES)

build/test_http_request_asan: $(SOURCES) $(TEST_SOURCES) $(HEADERS)
	mkdir -p build
	$(CC) $(CFLAGS) $(ASAN_FLAGS) -o $@ $(SOURCES) $(TEST_SOURCES)

clean:
	rm -rf build

.PHONY: test asan clean
