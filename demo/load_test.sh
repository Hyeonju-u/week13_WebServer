#!/bin/bash
# wrk(과제 문서가 든 부하 도구)로 멀티 루프와 단일 루프를 같은 조건에서 비교한다.
# 서버와 wrk가 CPU를 뺏지 않도록 코어를 나눈다: 서버 0~8번(9개), wrk 9~17번(9개).
# 멀티 루프는 서버가 쓰는 코어 수에 맞춰 --loops 9로 띄운다.
# wrk는 webserver-stable 이미지에 든 것(시계를 CLOCK_MONOTONIC으로 고친 판)을 쓴다.
# 2초 안에 응답이 없으면 wrk가 타임아웃으로 센다 (wrk 기본값).
# 결과: demo/results/<loops>loop_c<연결 수>.txt (wrk 원본), demo/results/summary.json, demo/results/demo.html
# 사용법: demo/load_test.sh            (Git Bash 또는 Linux, Docker 필요)
#   DURATION=10 LEVELS="100 1000" demo/load_test.sh  처럼 줄여서 돌릴 수 있다
set -e
cd "$(dirname "$0")/.."
WORK=$(pwd -W 2>/dev/null || pwd)
export MSYS_NO_PATHCONV=1
DURATION=${DURATION:-20}
LEVELS=${LEVELS:-"100 1000 10000"}
LOOP_SETS=${LOOP_SETS:-"9 1"}
SERVER_CPUS=${SERVER_CPUS:-0-8}
LOAD_CPUS=${LOAD_CPUS:-9-17}
LOAD_THREADS=${LOAD_THREADS:-8}
mkdir -p demo/results
rm -f demo/results/*loop_c*.json

stop_server () {
    docker rm -f demo-server >/dev/null 2>&1 || true
}
trap stop_server EXIT

# 벤치와 같은 본문(약 1.8KB)의 index.html을 서빙한다
start_server () {
    stop_server
    docker run -d --name demo-server --cpuset-cpus "$SERVER_CPUS" \
        --ulimit nofile=65536:65536 -v "$WORK:/work" webserver-stable \
        bash -c "make -s all >/dev/null && mkdir -p /tmp/www \
            && python3 -c \"print('<html><body>' + 'hello ' * 300 + '</body></html>')\" > /tmp/www/index.html \
            && exec ./build/webserver --root /tmp/www --port 8080 --loops $1 --log-level error" >/dev/null
    for i in $(seq 1 60); do
        docker exec demo-server curl -sf -o /dev/null http://127.0.0.1:8080/index.html && return 0
        sleep 1
    done
    echo "서버가 뜨지 않음"; docker logs demo-server; return 1
}

for loops in $LOOP_SETS; do
    start_server "$loops"
    for level in $LEVELS; do
        echo "루프 ${loops}개, 동시 연결 ${level}, ${DURATION}초"
        docker run --rm --network container:demo-server --cpuset-cpus "$LOAD_CPUS" \
            --ulimit nofile=65536:65536 webserver-stable \
            wrk -t"$LOAD_THREADS" -c"$level" -d"${DURATION}s" --latency \
            http://127.0.0.1:8080/index.html > "demo/results/${loops}loop_c${level}.txt"
    done
done
python3 demo/summarize.py demo/results > demo/results/summary.json
python3 demo/build_page.py
cat demo/results/summary.json
echo "비교 페이지: demo/results/demo.html"
