#!/usr/bin/env python3
"""wrk 원본 출력(tests/results/wrk_c*_run*.txt)에서 단계별 중앙값을 뽑아 표로 만들고 합격 여부를 판정한다.

합격 기준(환경변수로 바꿀 수 있다). 단계마다 모두 만족해야 합격이다.
  MAX_ERRORS   wrk 에러(소켓 오류 + 2xx/3xx 아닌 응답)의 회차 합계. 기본 0
  MIN_RPS      처리량 중앙값의 하한(req/s). 기본 30000
  P99_FACTOR   p99 중앙값 ≤ P99_FACTOR × 연결수 ÷ MIN_RPS(초). 기본 4
               연결이 많으면 줄이 길어져 지연이 늘어나는 것(연결수 ÷ 처리량)은 정상이므로 연결수에 비례해 허용한다.
하나라도 불합격이면 종료 코드 1을 돌려준다. 회차별 처리량·에러도 함께 보여 준다.
"""
import glob
import os
import re
import statistics
import sys

MAX_ERRORS = int(os.environ.get("MAX_ERRORS", "0"))
MIN_RPS = float(os.environ.get("MIN_RPS", "30000"))
P99_FACTOR = float(os.environ.get("P99_FACTOR", "4"))


def to_milliseconds(text):
    number = float(re.match(r"[\d.]+", text).group(0))
    for suffix, scale in (("us", 0.001), ("ms", 1.0), ("s", 1000.0)):
        if text.endswith(suffix):
            return number * scale
    return number


def parse(path):
    text = open(path).read()
    rps = float(re.search(r"Requests/sec:\s+([\d.]+)", text).group(1))
    p50 = to_milliseconds(re.search(r"\s50%\s+([\d.]+\w+)", text).group(1))
    p99 = to_milliseconds(re.search(r"\s99%\s+([\d.]+\w+)", text).group(1))
    errors = re.search(r"Socket errors: connect (\d+), read (\d+), write (\d+), timeout (\d+)", text)
    error_count = sum(int(v) for v in errors.groups()) if errors else 0
    non_ok = re.search(r"Non-2xx or 3xx responses: (\d+)", text)
    error_count += int(non_ok.group(1)) if non_ok else 0
    return rps, p50, p99, error_count


def judge(level, rows):
    """(합격 여부, 불합격 이유 목록)"""
    reasons = []
    errors = sum(r[3] for r in rows)
    rps = statistics.median(r[0] for r in rows)
    p99 = statistics.median(r[2] for r in rows)
    p99_limit = P99_FACTOR * level / MIN_RPS * 1000
    if errors > MAX_ERRORS:
        reasons.append("에러 %d건 > %d건" % (errors, MAX_ERRORS))
    if rps < MIN_RPS:
        reasons.append("처리량 %.0f < %.0f" % (rps, MIN_RPS))
    if p99 > p99_limit:
        reasons.append("p99 %.1fms > %.1fms" % (p99, p99_limit))
    return not reasons, reasons


def main(directory):
    levels = sorted({int(re.search(r"wrk_c(\d+)_", p).group(1)) for p in glob.glob(os.path.join(directory, "wrk_c*_run*.txt"))})
    print("합격 기준: 에러 ≤ %d건, 처리량 중앙값 ≥ %.0f req/s, p99 중앙값 ≤ %.0f × 연결수 ÷ %.0f (초)\n"
          % (MAX_ERRORS, MIN_RPS, P99_FACTOR, MIN_RPS))
    print("| 동시 연결 | 측정 횟수 | 처리량 중앙값(req/s) | 처리량 최소~최대 | p50 중앙값(ms) | p99 중앙값(ms) | 에러 합계 | 회차별 에러 | 판정 |")
    print("|---|---|---|---|---|---|---|---|---|")
    failed = False
    for level in levels:
        rows = [parse(p) for p in sorted(glob.glob(os.path.join(directory, "wrk_c%d_run*.txt" % level)))]
        passed, reasons = judge(level, rows)
        failed = failed or not passed
        rps = [r[0] for r in rows]
        print("| %d | %d | %.0f | %.0f~%.0f | %.2f | %.2f | %d | %s | %s |" % (
            level, len(rows), statistics.median(rps), min(rps), max(rps),
            statistics.median(r[1] for r in rows), statistics.median(r[2] for r in rows),
            sum(r[3] for r in rows), ", ".join(str(r[3]) for r in rows),
            "통과" if passed else "**불합격** (" + "; ".join(reasons) + ")"))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1] if len(sys.argv) > 1 else "tests/results"))
