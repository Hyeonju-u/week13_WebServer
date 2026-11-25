#!/usr/bin/env python3
"""용량 시험 두 가지.

cap   연결 상한(--max-connections)에 실제로 닿는지, 닿은 뒤 넘치지 않는지, 줄면 대기하던 손님을 받는지.
      stress_test.py의 TST-11 상한 초과(wrk 12,000개)는 fd 6,435까지만 올라 상한에 닿지 못했다.
      여기서는 상한 5,000을 정확히 채운 뒤 넘치는 연결을 시도한다.
proxy 프록시 경로를 지속 부하(wrk)로 몰고, 응답 코드를 세어 서버가 망가지지 않는지 본다.
      뒤쪽 서버도 이 프로젝트의 웹서버를 쓴다(빠르게 답하므로 프록시 쪽 한계가 드러난다).

사용법: python3 tests/capacity_test.py [--only cap|proxy]
처리량·502 비율·CPU는 환경에 따라 크게 흔들리므로 기록만 하고 합격 기준으로 삼지 않는다.
"""
import argparse
import asyncio
import json
import os
import re
import resource
import shutil
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import Checker, Server, count_extra_loop_fds, get, LOCALHOST  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
STATUS_SCRIPT = os.path.join(HERE, "proxy_status.lua")
checker = Checker()
check = checker.check
RESULTS = []

PARTIAL_REQUEST = b"GET /small.txt HTTP/1.1\r\nHost: t\r\n"
FULL_REQUEST = PARTIAL_REQUEST + b"\r\n"


def record(name, **values):
    RESULTS.append(dict(name=name, **values))


def raise_client_fd_limit():
    hard = resource.getrlimit(resource.RLIMIT_NOFILE)[1]
    for target in (65000, 30000, 20000, 14000, 11000):
        if hard != resource.RLIM_INFINITY and target > hard:
            continue
        try:
            resource.setrlimit(resource.RLIMIT_NOFILE, (target, hard))
            return target
        except (ValueError, OSError):
            continue
    return resource.getrlimit(resource.RLIMIT_NOFILE)[0]


def make_root(base):
    root = tempfile.mkdtemp(prefix="root_", dir=base)
    os.makedirs(os.path.join(root, "api"))
    page = "<html><body>" + "hello " * 300 + "</body></html>"
    for name in ("index.html", os.path.join("api", "index.html")):
        with open(os.path.join(root, name), "w") as handle:
            handle.write(page)
    with open(os.path.join(root, "small.txt"), "w") as handle:
        handle.write("small")
    return root


async def open_connection(port, timeout):
    return await asyncio.wait_for(asyncio.open_connection(LOCALHOST, port, limit=1 << 20), timeout)


async def read_status(reader):
    head = await reader.readuntil(b"\r\n\r\n")
    lines = head.decode("latin-1").split("\r\n")
    status = int(lines[0].split(" ")[1])
    length = 0
    for line in lines[1:]:
        if line.lower().startswith("content-length:"):
            length = int(line.split(":")[1])
    body = await reader.readexactly(length) if length else b""
    return status, body


async def open_many(port, count, wave=100, timeout=30):
    """listen 대기열(macOS 기본 128)을 넘지 않게 wave개씩 나눠 연결한다."""
    pairs = []
    errors = 0
    remaining = count
    while remaining > 0:
        size = min(wave, remaining)
        remaining -= size
        results = await asyncio.gather(*[open_connection(port, timeout) for _ in range(size)],
                                       return_exceptions=True)
        for result in results:
            if isinstance(result, Exception):
                errors += 1
            else:
                pairs.append(result)
    return pairs, errors


# ---------- cap: 연결 상한에 실제로 닿기 ----------
async def scenario_cap(base, cap, extra_count, free_count):
    """요청을 끝내지 않은 연결로 상한을 채우고(요청 읽기 15초 안에 끝내야 한다), 넘치는 연결을 시도한다."""
    root = make_root(base)
    server = Server(root, max_connections=cap, fd_limit=256)
    baseline = server.fd_count()
    started = time.time()

    held, errors = await open_many(server.port, cap)
    for reader, writer in held:
        writer.write(PARTIAL_REQUEST)
    await asyncio.sleep(1)
    check("상한 %d개 연결이 모두 성립 (실패 %d)" % (cap, errors), len(held) == cap and errors == 0)

    peak = server.fd_count()
    check("서버 fd가 상한에 닿음 (%d ≤ fd %d ≤ %d)" % (cap, peak, cap + 30 + count_extra_loop_fds()), cap <= peak <= cap + 30 + count_extra_loop_fds())

    extras = []
    for _ in range(extra_count):
        extras.append(open_connection(server.port, 2))
    connected = [r for r in await asyncio.gather(*extras, return_exceptions=True) if not isinstance(r, Exception)]
    for reader, writer in connected:
        writer.write(FULL_REQUEST)
    tasks = [asyncio.ensure_future(read_status(reader)) for reader, _ in connected]
    await asyncio.sleep(2)
    served_while_full = sum(1 for t in tasks if t.done() and not t.exception())
    peak_after = server.fd_count()
    check("상한이 찬 동안 넘치는 연결 %d개(대기열에 들어간 것)는 처리되지 않음 (처리됨 %d)"
          % (len(connected), served_while_full), served_while_full == 0)
    check("넘치는 연결을 시도한 뒤에도 서버 fd가 상한 이하 (fd %d ≤ %d)" % (peak_after, cap + 30 + count_extra_loop_fds()),
          peak_after <= cap + 30 + count_extra_loop_fds())

    for reader, writer in held[:free_count]:
        writer.close()
    deadline = time.time() + 5
    while time.time() < deadline and not any(t.done() for t in tasks):
        await asyncio.sleep(0.2)
    await asyncio.sleep(1)
    results = [t.result() for t in tasks if t.done() and not t.exception()]
    wrong = sum(1 for r in results if r != (200, b"small"))
    check("연결이 %d개 줄자 대기하던 연결을 받아 응답 (응답받음 %d / 대기 %d)"
          % (free_count, len(results), len(connected)), len(results) > 0)
    check("그 응답이 모두 정확함 (잘못된 응답 %d)" % wrong, wrong == 0)
    elapsed = time.time() - started
    check("모든 확인이 요청 읽기 제한(15초) 안에 끝남 (%.1f초)" % elapsed, elapsed < 14)

    for task in tasks:
        task.cancel()
    for reader, writer in held[free_count:] + connected:
        writer.close()
    deadline = time.time() + 30
    final = server.fd_count()
    while time.time() < deadline and final > baseline + 2:
        await asyncio.sleep(2)
        final = server.fd_count()
    check("서버 생존, 연결을 모두 닫으면 fd 복구 (전 %d → 후 %d)" % (baseline, final),
          server.is_alive and final <= baseline + 2)
    check("서버 로그에 ERROR 없음", "[ERROR]" not in server.log_text())
    record("cap", max_connections=cap, peak_fd=peak, peak_fd_after_extras=peak_after,
           extras_connected=len(connected), served_while_full=served_while_full,
           served_after_free=len(results), elapsed=round(elapsed, 1))
    server.stop()


# ---------- proxy: 프록시 경로 지속 부하 ----------
def count_time_wait():
    result = subprocess.run("netstat -an -p tcp | grep -c TIME_WAIT", shell=True, capture_output=True, text=True)
    return int(result.stdout.strip() or 0)


def wait_time_wait_drain(limit=500, timeout=70):
    """앞선 부하가 남긴 TIME_WAIT가 임시 포트를 차지하고 있으면 결과가 흔들리므로 비워질 때까지 기다린다."""
    deadline = time.time() + timeout
    count = count_time_wait()
    while count >= limit and time.time() < deadline:
        time.sleep(3)
        count = count_time_wait()
    return count


def run_wrk_with_status(port, path, connections, seconds):
    command = ["wrk", "-t2", "-c%d" % connections, "-d%ds" % seconds, "--latency", "-s", STATUS_SCRIPT,
               "http://%s:%d%s" % (LOCALHOST, port, path)]
    output = subprocess.run(command, capture_output=True, text=True, timeout=seconds + 60).stdout
    codes = {int(code): int(count) for code, count in re.findall(r"상태 (\d+): (\d+)건", output)}
    match = re.search(r"Requests/sec:\s+([\d.]+)", output)
    errors = re.search(r"Socket errors: connect (\d+), read (\d+), write (\d+), timeout (\d+)", output)
    return dict(codes=codes, rps=float(match.group(1)) if match else 0.0,
                socket_errors=sum(int(v) for v in errors.groups()) if errors else 0)


def scenario_proxy_load(base, connections, seconds):
    if shutil.which("wrk") is None:
        print("  (wrk가 없어 proxy 시험을 건너뜀)")
        return
    root = make_root(base)
    backend = Server(root)
    config = os.path.join(base, "proxy.conf")
    with open(config, "w") as handle:
        handle.write("route /api 127.0.0.1:%d\n" % backend.port)
    proxy = Server(root, config=config)
    drained = wait_time_wait_drain()
    before = dict(fd=proxy.fd_count(), rss=proxy.memory_kb())
    proxy_cpu = proxy.cpu_seconds()
    backend_cpu = backend.cpu_seconds()
    started = time.time()
    result = run_wrk_with_status(proxy.port, "/api/index.html", connections, seconds)
    elapsed = time.time() - started
    proxy_percent = (proxy.cpu_seconds() - proxy_cpu) / elapsed * 100
    backend_percent = (backend.cpu_seconds() - backend_cpu) / elapsed * 100
    codes = result["codes"]
    total = sum(codes.values())
    ok = codes.get(200, 0)
    bad = codes.get(502, 0)
    other = total - ok - bad

    check("프록시와 뒤쪽 서버가 부하 중 죽지 않음", proxy.is_alive and backend.is_alive)
    check("프록시 경유로 200 응답이 실제로 나옴 (200: %d건 / 전체 %d건)" % (ok, total), ok > 0)
    check("응답 코드가 200 또는 502뿐 (그 밖의 코드 %d건: %s)" % (other, sorted(codes)), other == 0)
    ratio = bad / total if total else 1.0
    check("502 비율이 절반 미만 (%.1f%%, 시작 때 TIME_WAIT %d개)" % (ratio * 100, drained), ratio < 0.5)

    after_drain = wait_time_wait_drain()
    response = get(proxy.port, "/api/index.html")
    check("부하가 끝나고 TIME_WAIT가 비워지면 프록시 경유 요청이 다시 200 (TIME_WAIT %d개, 상태 %s)"
          % (after_drain, response.status if response else None), response is not None and response.status == 200)
    after = dict(fd=proxy.fd_count(), rss=proxy.memory_kb())
    check("프록시 fd 복구 (전 %d → 후 %d)" % (before["fd"], after["fd"]), after["fd"] <= before["fd"] + 2)
    check("프록시 메모리가 늘지 않음 (전 %d KB → 후 %d KB, 여유 16MB)" % (before["rss"], after["rss"]),
          after["rss"] <= before["rss"] + 16384)
    leaks = proxy.count_leaks()
    if leaks is not None:
        check("프록시 누수 검사(leaks): %d건, %d바이트" % leaks, leaks[0] == 0)
    check("프록시 로그에 ERROR 없음", "[ERROR]" not in proxy.log_text())
    print("  참고(합격 기준 아님): 처리량 %.0f req/s, 502 %.1f%%, 프록시 CPU %.0f%%, 뒤쪽 서버 CPU %.0f%%, 소켓 오류 %d"
          % (result["rps"], ratio * 100, proxy_percent, backend_percent, result["socket_errors"]))
    record("proxy", connections=connections, seconds=seconds, codes={str(k): v for k, v in codes.items()},
           rps=result["rps"], ratio_502=round(ratio, 4), proxy_cpu_percent=round(proxy_percent),
           backend_cpu_percent=round(backend_percent), socket_errors=result["socket_errors"],
           time_wait_at_start=drained)
    proxy.stop()
    backend.stop()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--only", choices=("cap", "proxy"), default="")
    parser.add_argument("--cap", type=int, default=5000)
    arguments = parser.parse_args()
    limit = raise_client_fd_limit()
    print("클라이언트 fd 한도:", limit)
    base = tempfile.mkdtemp(prefix="ws_capacity_")
    try:
        if arguments.only in ("", "cap"):
            print("[연결 상한 %d에 실제로 닿기]" % arguments.cap)
            asyncio.run(scenario_cap(base, arguments.cap, extra_count=200, free_count=500))
        if arguments.only in ("", "proxy"):
            print("[프록시 경로 지속 부하]")
            scenario_proxy_load(base, connections=100, seconds=10)
    finally:
        shutil.rmtree(base, ignore_errors=True)
        results_dir = os.path.join(HERE, "results")
        os.makedirs(results_dir, exist_ok=True)
        with open(os.path.join(results_dir, "capacity_test.json"), "w") as handle:
            json.dump(RESULTS, handle, indent=1, ensure_ascii=False)
    return checker.finish()


if __name__ == "__main__":
    sys.exit(main())
