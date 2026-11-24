# (B13) make test: 일반 빌드 / make asan: ASan·LSan 빌드 (Q5)
# (S30) 1단계·2단계 실행 파일을 차례로 돌리고, 실패하면 멈춘다
CC = gcc
CFLAGS = -Wall -Wextra -std=c11 -g -D_XOPEN_SOURCE=700 -Iinclude
ASAN_FLAGS = -fsanitize=address,leak -fno-omit-frame-pointer

REQUEST_SOURCES = src/http_request.c
STATIC_SOURCES = src/http_request.c src/http_response.c src/static_file.c
HEADERS = include/http_request.h include/http_response.h \
          include/static_file.h tests/check.h

test: build/test_http_request build/test_static_file
	./build/test_http_request
	./build/test_static_file

asan: build/test_http_request_asan build/test_static_file_asan
	./build/test_http_request_asan
	./build/test_static_file_asan

build/test_http_request: $(REQUEST_SOURCES) tests/test_http_request.c $(HEADERS)
	mkdir -p build
	$(CC) $(CFLAGS) -o $@ $(REQUEST_SOURCES) tests/test_http_request.c

build/test_http_request_asan: $(REQUEST_SOURCES) tests/test_http_request.c \
                              $(HEADERS)
	mkdir -p build
	$(CC) $(CFLAGS) $(ASAN_FLAGS) -o $@ $(REQUEST_SOURCES) \
		tests/test_http_request.c

build/test_static_file: $(STATIC_SOURCES) tests/test_static_file.c $(HEADERS)
	mkdir -p build
	$(CC) $(CFLAGS) -o $@ $(STATIC_SOURCES) tests/test_static_file.c

build/test_static_file_asan: $(STATIC_SOURCES) tests/test_static_file.c \
                             $(HEADERS)
	mkdir -p build
	$(CC) $(CFLAGS) $(ASAN_FLAGS) -o $@ $(STATIC_SOURCES) \
		tests/test_static_file.c

clean:
	rm -rf build

.PHONY: test asan clean
