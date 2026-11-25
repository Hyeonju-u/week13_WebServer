# Linux(epoll)에서 같은 테스트를 돌리려고 만든 개발용 이미지.
# 사용법은 README "Linux에서 재현" 절 참고.
FROM ubuntu:24.04

# procps: 유휴 CPU 측정(ps). libssl-dev·unzip·git은 wrk 빌드에만 쓴다
RUN apt-get update \
    && apt-get install -y --no-install-recommends \
       build-essential python3 curl valgrind procps ca-certificates \
       git libssl-dev unzip \
    && rm -rf /var/lib/apt/lists/*

# wrk는 지연을 벽시계(gettimeofday)로 잰다. Docker Desktop(WSL2) VM은 벽시계가
# 수십 ms씩 뒤로 가서, 그 순간 처리 중이던 요청의 지연이 음수 → 부호 없는 큰 값이
# 되어 연결 수만큼 "timeout"으로 세진다(최대 지연 2ms인데 timeout 20).
# macOS 측정과 같은 4.2.0을 받아 시간 함수만 단조 시계로 바꿔 빌드한다.
# 바뀌지 않았으면 grep이 실패해 빌드가 멈춘다
RUN git clone --depth 1 --branch 4.2.0 https://github.com/wg/wrk.git /tmp/wrk \
    && sed -i '/static uint64_t time_us/,/^}/{s/struct timeval t;/struct timespec t;/;s/gettimeofday(&t, NULL);/clock_gettime(CLOCK_MONOTONIC, \&t);/;s/t.tv_usec;/t.tv_nsec \/ 1000;/}' /tmp/wrk/src/wrk.c \
    && grep -q "clock_gettime(CLOCK_MONOTONIC, &t);" /tmp/wrk/src/wrk.c \
    && make -C /tmp/wrk -j"$(nproc)" \
    && cp /tmp/wrk/wrk /usr/local/bin/wrk \
    && rm -rf /tmp/wrk

# ubuntu:24.04에 이미 있는 일반 사용자. root로 돌리면 fd 한도·권한 시험이
# 실제 환경과 달라진다
USER ubuntu
WORKDIR /work
