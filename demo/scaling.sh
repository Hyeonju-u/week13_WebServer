#!/bin/bash
# 루프 수를 바꿔 가며 처리량이 어떻게 늘어나는지 잰다 (확장 곡선).
# 모든 측정에서 서버에는 같은 코어 9개(0~8번)를 주고, 루프 수만 바꾼다.
# 측정 중 서버 코어가 실제로 몇 개분 일했는지도 /proc/stat으로 함께 잰다.
# 결과: demo/results/scaling/loops<N>.txt (wrk 원본), demo/results/scaling.json
# 사용법: demo/scaling.sh     (LOOP_SETS="1 2 4 9" CONNECTIONS=1000 DURATION=15 로 바꿀 수 있다)
set -e
cd "$(dirname "$0")/.."
WORK=$(pwd -W 2>/dev/null || pwd)
export MSYS_NO_PATHCONV=1
LOOP_SETS=${LOOP_SETS:-"1 2 3 4 6 9"}
CONNECTIONS=${CONNECTIONS:-1000}
DURATION=${DURATION:-15}
OUT=demo/results/scaling
mkdir -p "$OUT"

stop_server () {
    docker rm -f demo-server >/dev/null 2>&1 || true
}
trap stop_server EXIT

start_server () {
    stop_server
    docker run -d --name demo-server --cpuset-cpus 0-8 \
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

# 코어 0~8번의 (전체, 쉰) 시간 합계
read_cpu_ticks () {
    docker exec demo-server awk '$1 ~ /^cpu[0-8]$/ {
        total += $2+$3+$4+$5+$6+$7+$8+$9; idle += $5+$6 } END { print total, idle }' /proc/stat
}

for loops in $LOOP_SETS; do
    start_server "$loops"
    echo "루프 ${loops}개, 동시 연결 ${CONNECTIONS}, ${DURATION}초"
    before=$(read_cpu_ticks)
    docker run --rm --network container:demo-server --cpuset-cpus 9-17 \
        --ulimit nofile=65536:65536 webserver-stable \
        wrk -t8 -c"$CONNECTIONS" -d"${DURATION}s" --latency \
        http://127.0.0.1:8080/index.html > "$OUT/loops${loops}.txt"
    after=$(read_cpu_ticks)
    echo "$before $after" | awk '{ total = $3 - $1; idle = $4 - $2;
        printf "busy_cores %.2f\n", (total > 0 ? (total - idle) / total * 9 : 0) }' >> "$OUT/loops${loops}.txt"
done
python3 demo/summarize_scaling.py "$OUT" > demo/results/scaling.json
cat demo/results/scaling.json
