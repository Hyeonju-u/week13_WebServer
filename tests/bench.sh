#!/bin/sh
# 부하 테스트(TST-03): 동시 연결 100 → 1,000 → 10,000 단계, 단계마다 30초씩 3회.
# 성능 수치는 ASan을 끈 일반 빌드로 잰다 (TST-06). 원본 출력은 tests/results/에 저장한다.
# 처리량을 잰 뒤 짧은 별도 실행으로 응답 본문(상태 200, 길이)도 검사한다. Lua 응답 검사는 wrk를 느리게 하므로 따로 돌린다.
# 합격 기준은 tests/summarize_wrk.py에 있고, 하나라도 불합격이면 이 스크립트도 실패(종료 코드 1)한다.
# 사용법: tests/bench.sh [서버 바이너리]
set -e
cd "$(dirname "$0")/.."
BINARY=${1:-build/webserver}
PORT=${PORT:-18700}
DURATION=${DURATION:-30}
RUNS=${RUNS:-3}
LEVELS=${LEVELS:-"100 1000 10000"}
RESULTS=${RESULTS:-tests/results}
BODY_CHECK_SECONDS=${BODY_CHECK_SECONDS:-5}

# wrk도 연결마다 fd를 쓰므로 이 터미널의 fd 한도도 올린다
ulimit -n 65000 2>/dev/null || ulimit -n "$(ulimit -Hn)"
mkdir -p "$RESULTS"
ROOT=$(mktemp -d)
python3 -c "print('<html><body>' + 'hello ' * 300 + '</body></html>')" > "$ROOT/index.html"
BENCH_BODY_BYTES=$(wc -c < "$ROOT/index.html" | tr -d ' ')
export BENCH_BODY_BYTES

"$BINARY" --root "$ROOT" --port "$PORT" --log-level error &
SERVER_PID=$!
trap 'kill -INT $SERVER_PID 2>/dev/null; rm -rf "$ROOT"' EXIT
sleep 1

for level in $LEVELS; do
    for run in $(seq 1 "$RUNS"); do
        echo "연결 $level, ${run}회차"
        wrk -t4 -c"$level" -d"${DURATION}s" --latency \
            "http://127.0.0.1:$PORT/index.html" > "$RESULTS/wrk_c${level}_run${run}.txt"
    done
done

echo "본문 검사 (${BODY_CHECK_SECONDS}초, 연결 100)"
wrk -t2 -c100 -d"${BODY_CHECK_SECONDS}s" -s tests/bench_body.lua \
    "http://127.0.0.1:$PORT/index.html" > "$RESULTS/wrk_body_check.txt"

STATUS=0
python3 tests/summarize_wrk.py "$RESULTS" > "$RESULTS/wrk_summary.md" || STATUS=$?
cat "$RESULTS/wrk_summary.md"
echo
BODY_LINE=$(grep "본문 검사" "$RESULTS/wrk_body_check.txt" || true)
echo "${BODY_LINE:-본문 검사 결과 없음}"
case "$BODY_LINE" in
    *"검사 0건"*|"") echo "본문 검사 불합격: 검사한 응답이 없음"; STATUS=1 ;;
    *"불일치 0건"*) echo "본문 검사 통과" ;;
    *) echo "본문 검사 불합격"; STATUS=1 ;;
esac
exit $STATUS
