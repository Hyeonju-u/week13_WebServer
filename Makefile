CC ?= cc
UNAME := $(shell uname -s)
ifeq ($(UNAME),Darwin)
FEATURE = -D_DARWIN_C_SOURCE
else
FEATURE = -D_POSIX_C_SOURCE=200809L
endif
CFLAGS ?= -O2
CFLAGS += -std=c11 $(FEATURE) -Wall -Wextra -Wpedantic -Werror -Isrc

SERVER_SOURCES = $(filter-out src/main.c,$(wildcard src/*.c))
SERVER_OBJECTS = $(patsubst src/%.c,build/%.o,$(SERVER_SOURCES))
ASAN_FLAGS = -fsanitize=address,undefined -fno-omit-frame-pointer -g -O1
UBSAN_FLAGS = -fsanitize=undefined -fno-sanitize-recover=undefined -g -O1
UNIT_TESTS = $(patsubst tests/%.c,build/%,$(wildcard tests/test_*.c))

.PHONY: all asan asan-test ubsan limit-build test unit-test functional-test backend-health-test stress-test bench clean

all: build/webserver

build/webserver: src/main.c $(SERVER_OBJECTS)
	$(CC) $(CFLAGS) -o $@ $^

build/%.o: src/%.c $(wildcard src/*.h) | build
	$(CC) $(CFLAGS) -c -o $@ $<

build:
	mkdir -p build

# 버퍼 합계 상한(NET-12)의 503 경로를 확인하려고 상한을 3MB로 줄인 시험용 빌드
limit-build:
	mkdir -p build-limit
	$(CC) $(CFLAGS) -DBUFFER_TOTAL_MAX=3145728 -o build-limit/webserver src/*.c

# 정확성·메모리 검사용 빌드. 성능 수치는 ASan을 끈 일반 빌드로 잰다 (TST-06)
asan:
	mkdir -p build-asan
	$(CC) $(CFLAGS) $(ASAN_FLAGS) -o build-asan/webserver src/*.c

# ASan 빌드로 기능 테스트를 돌린다. 기능 테스트는 로그의 [ERROR]만 보므로 UBSan의
# "runtime error"처럼 멈추지 않는 보고를 놓친다. 그래서 sanitizer 보고를 파일로 받아
# 하나라도 생기면 실패로 본다. 누수는 서버가 SIGINT로 끝날 때 LeakSanitizer가 보고한다.
# ASan은 free한 메모리를 격리 구역(기본 256MB)에 쌓아 두어 RSS가 늘어서, 배압 시험의
# "메모리 3MB 미만 증가"가 ASan 몫 때문에 실패한다(기본 +5.6MB, 0이면 +1.9MB).
# 0이면 해제 후 사용 검출이 거의 사라지므로 방금 free한 1MB만 격리한다
ASAN_REPORTS = build-asan/reports
ASAN_QUARANTINE_MB = 1
asan-test: asan limit-build
	rm -rf $(ASAN_REPORTS)
	mkdir -p $(ASAN_REPORTS)
	ASAN_OPTIONS=log_path=$(CURDIR)/$(ASAN_REPORTS)/asan:detect_leaks=1:quarantine_size_mb=$(ASAN_QUARANTINE_MB) \
	UBSAN_OPTIONS=log_path=$(CURDIR)/$(ASAN_REPORTS)/ubsan:print_stacktrace=1 \
	WEBSERVER_BINARY=$(CURDIR)/build-asan/webserver \
	sh -c 'python3 tests/functional_test.py && python3 tests/backend_health_test.py'
	@if [ -n "$$(ls $(ASAN_REPORTS))" ]; then \
	    echo "sanitizer 보고 있음:"; cat $(ASAN_REPORTS)/*; exit 1; \
	fi
	@echo "sanitizer 보고 0건 (누수 포함)"

# ASan이 동작하지 않는 환경(이 개발 컴퓨터)에서 쓰는 대체 빌드
ubsan:
	mkdir -p build-ubsan
	$(CC) $(CFLAGS) $(UBSAN_FLAGS) -o build-ubsan/webserver src/*.c

build/test_%: tests/test_%.c $(SERVER_OBJECTS)
	$(CC) $(CFLAGS) -Itests -o $@ $< $(SERVER_OBJECTS)

unit-test: $(UNIT_TESTS)
	for test in $(UNIT_TESTS); do ./$$test || exit 1; done

# 모든 테스트를 명령 하나로 다시 실행한다 (TST-07). 가혹 조건은 규모를 줄인 빠른 판이다.
# 정식 규모는 make stress-test, 30분 장시간은 make long-test
test: all unit-test functional-test backend-health-test
	python3 tests/stress_test.py --quick

functional-test: all limit-build
	python3 tests/functional_test.py

backend-health-test: all
	python3 tests/backend_health_test.py

stress-test: all
	python3 tests/stress_test.py

long-test: all
	python3 tests/stress_test.py --only 20 --long-minutes 30

bench: all
	tests/bench.sh

clean:
	rm -rf build build-asan build-ubsan build-limit
