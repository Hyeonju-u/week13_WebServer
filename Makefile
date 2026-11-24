# (B13) make test: 일반 빌드 / make asan: ASan·LSan 빌드 (Q5)
# (S30) 1단계·2단계 실행 파일을 차례로 돌리고, 실패하면 멈춘다
# (L40) make server / server-asan: 3단계 서버, make itest / itest-asan: 통합 테스트
CC = gcc
CFLAGS = -Wall -Wextra -std=c11 -g -D_XOPEN_SOURCE=700 -Iinclude
ASAN_FLAGS = -fsanitize=address,leak -fno-omit-frame-pointer
# (L19, L40) 서버는 끝나는 방법이 없어 leak 검사는 6단계에서
SERVER_ASAN_FLAGS = -fsanitize=address -fno-omit-frame-pointer

REQUEST_SOURCES = src/http_request.c
STATIC_SOURCES = src/http_request.c src/http_response.c src/static_file.c
SERVER_SOURCES = $(STATIC_SOURCES) src/timer_list.c src/access_log.c \
                 src/connection.c src/event_loop.c src/main.c
HEADERS = include/http_request.h include/http_response.h \
          include/static_file.h tests/check.h
SERVER_HEADERS = include/http_request.h include/http_response.h \
                 include/static_file.h include/timer_list.h \
                 include/access_log.h include/connection.h \
                 include/event_loop.h
INTEGRATION_TEST = tests/test_event_loop.py

test: build/test_http_request build/test_static_file
	./build/test_http_request
	./build/test_static_file

asan: build/test_http_request_asan build/test_static_file_asan
	./build/test_http_request_asan
	./build/test_static_file_asan

server: build/server

server-asan: build/server_asan

itest: build/server
	python3 $(INTEGRATION_TEST) build/server

itest-asan: build/server_asan
	ASAN_OPTIONS=detect_leaks=0 python3 $(INTEGRATION_TEST) \
		build/server_asan

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

build/server: $(SERVER_SOURCES) $(SERVER_HEADERS)
	mkdir -p build
	$(CC) $(CFLAGS) -o $@ $(SERVER_SOURCES)

build/server_asan: $(SERVER_SOURCES) $(SERVER_HEADERS)
	mkdir -p build
	$(CC) $(CFLAGS) $(SERVER_ASAN_FLAGS) -o $@ $(SERVER_SOURCES)

clean:
	rm -rf build

.PHONY: test asan server server-asan itest itest-asan clean
