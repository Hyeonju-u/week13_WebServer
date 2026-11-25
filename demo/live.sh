#!/bin/bash
# 발표 때 터미널에서 보여 주는 실시간 데모.
# wrk가 요청을 보내는 동안 서버 코어 9개의 사용률을 막대로 그리고, 끝나면 wrk 결과를 보여 준다.
# 단일 루프는 코어 하나만 꽉 차고, 멀티 루프는 9개가 다 찬다.
# 사용법: demo/live.sh <루프 수> [동시 연결 수] [초]
#   예) demo/live.sh 1 1000 15   → 단일 루프
#       demo/live.sh 9 1000 15   → 멀티 루프
set -e
LOOPS=${1:-9}
CONNECTIONS=${2:-1000}
SECONDS_TO_RUN=${3:-15}
cd "$(dirname "$0")/.."
WORK=$(pwd -W 2>/dev/null || pwd)
export MSYS_NO_PATHCONV=1

cleanup () {
    docker rm -f demo-server demo-wrk >/dev/null 2>&1 || true
}
cleanup
trap cleanup EXIT
docker run -d --name demo-server --cpuset-cpus 0-8 --ulimit nofile=65536:65536 \
    -v "$WORK:/work" webserver-stable \
    bash -c "make -s all >/dev/null && mkdir -p /tmp/www \
        && python3 -c \"print('<html><body>' + 'hello ' * 300 + '</body></html>')\" > /tmp/www/index.html \
        && exec ./build/webserver --root /tmp/www --port 8080 --loops $LOOPS --log-level error" >/dev/null
until docker exec demo-server curl -sf -o /dev/null http://127.0.0.1:8080/index.html; do
    sleep 1
done

docker run -d --name demo-wrk --network container:demo-server --cpuset-cpus 9-17 \
    --ulimit nofile=65536:65536 webserver-stable \
    wrk -t8 -c"$CONNECTIONS" -d"${SECONDS_TO_RUN}s" --latency \
    http://127.0.0.1:8080/index.html >/dev/null
# 막대는 wrk보다 1초 먼저 멈춘다. wrk가 끝난 뒤의 0% 화면이 마지막에 남지 않게 하려고
METER_SECONDS=$(( SECONDS_TO_RUN > 2 ? SECONDS_TO_RUN - 1 : 1 ))
docker exec demo-server python3 -u /work/demo/core_meter.py 0-8 "$METER_SECONDS" \
    "루프 ${LOOPS}개 · 동시 연결 ${CONNECTIONS}개 · wrk 실행 중"
docker wait demo-wrk >/dev/null
echo
echo "── wrk 결과 ──"
docker logs demo-wrk 2>&1 | grep -E "requests in|Requests/sec|Socket errors|Non-2xx|^ +(50|99)%"
