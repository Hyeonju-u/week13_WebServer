#!/usr/bin/env python3
"""가혹 조건 테스트 (11-5). 각 시나리오는 정확성 기준 6가지(11-3)를 확인하고,
지연을 재는 시나리오는 기준선 대비 2배 이내인지(11-4) 판정한다.

사용법: stress_test.py [--quick] [--only 11,12] [--long-minutes N]
"""
import argparse
import asyncio
import json
import os
import random
import re
import resource
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import (Checker, PythonBackend, Server, count_extra_loop_fds, find_free_port, fingerprint, LOCALHOST)

checker = Checker()
check = checker.check
RESULTS = []
LATENCY_FACTOR = 2.0


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


def record(name, **values):
    RESULTS.append(dict(name=name, **values))


def to_milliseconds(text):
    number = float(re.match(r"[\d.]+", text).group(0))
    if text.endswith("us"):
        return number / 1000
    if text.endswith("ms"):
        return number
    if text.endswith("s"):
        return number * 1000
    return number


def run_wrk(port, path="/index.html", connections=20, threads=2, seconds=8):
    command = ["wrk", "-t%d" % threads, "-c%d" % connections, "-d%ds" % seconds, "--latency",
               "http://%s:%d%s" % (LOCALHOST, port, path)]
    return command


def parse_wrk(output):
    result = {}
    match = re.search(r"Requests/sec:\s+([\d.]+)", output)
    result["rps"] = float(match.group(1)) if match else 0.0
    for label in ("50%", "99%"):
        match = re.search(r"\s%s\s+([\d.]+(?:us|ms|s))" % re.escape(label), output)
        result["p" + label[:-1]] = to_milliseconds(match.group(1)) if match else None
    match = re.search(r"Socket errors: connect (\d+), read (\d+), write (\d+), timeout (\d+)", output)
    result["socket_errors"] = sum(int(v) for v in match.groups()) if match else 0
    match = re.search(r"Non-2xx or 3xx responses: (\d+)", output)
    result["non_2xx"] = int(match.group(1)) if match else 0
    return result


async def measure_wrk(port, **kwargs):
    process = await asyncio.create_subprocess_exec(
        *run_wrk(port, **kwargs), stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.STDOUT)
    output, _ = await process.communicate()
    return parse_wrk(output.decode())


def judge_latency(label, baseline, loaded):
    """기준선 대비 2배 이내인지 판정한다. 기준선이 아주 작으면 측정 잡음이 크므로 0.3ms를 바닥으로 둔다."""
    ok = True
    for key in ("p50", "p99"):
        limit = max(baseline[key] * LATENCY_FACTOR, 0.3 * LATENCY_FACTOR)
        good = loaded[key] is not None and loaded[key] <= limit
        ok = ok and good
        check("%s 정상 손님 %s 지연이 기준선 %.2fms의 2배(%.2fms) 이내" % (label, key, baseline[key], limit),
              good, "(실제 %s ms)" % loaded[key])
    check("%s 정상 손님 응답 오류 0" % label, loaded["socket_errors"] == 0 and loaded["non_2xx"] == 0,
          "(소켓 오류 %d, 비정상 응답 %d)" % (loaded["socket_errors"], loaded["non_2xx"]))
    return ok


FILES = {}


def make_files(base):
    """시나리오마다 같은 파일을 쓰므로 100MB 파일은 한 번만 만든다."""
    if base not in FILES:
        FILES[base] = create_files(base)
    return FILES[base]


def create_files(base):
    root = os.path.join(base, "root")
    os.makedirs(root)
    with open(os.path.join(root, "index.html"), "w") as handle:
        handle.write("<html><body>" + "hello " * 100 + "</body></html>")
    for index in range(10):
        with open(os.path.join(root, "file_%d.txt" % index), "w") as handle:
            handle.write("content-of-file-%d" % index)
    with open(os.path.join(root, "small.txt"), "w") as handle:
        handle.write("small")
    big_path = os.path.join(root, "big100.bin")
    digest = __import__("hashlib").sha256()
    with open(big_path, "wb") as handle:
        for _ in range(100):
            block = os.urandom(1 << 20)
            digest.update(block)
            handle.write(block)
    return root, digest.hexdigest()


def snapshot(server):
    return dict(fd=server.fd_count(), rss=server.memory_kb())


def check_recovered(label, server, before, wait_seconds=1.0, memory_margin_kb=4096, stable_with=None):
    """정확성 기준 5, 6: 서버가 살아 있고 fd와 메모리가 시작 수준으로 돌아온다.
    할당자가 페이지를 늦게 비우므로 30초까지 지켜본다. stable_with를 주면 시작값 대신
    '같은 부하를 한 번 더 돌려도 늘지 않는다'로 판정한다(할당자가 쥔 조각과 누수를 구분)."""
    time.sleep(wait_seconds)
    deadline = time.time() + 30
    limit = (stable_with["rss"] if stable_with else before["rss"]) + memory_margin_kb
    after = snapshot(server)
    while time.time() < deadline and after["rss"] > limit:
        time.sleep(2)
        after = snapshot(server)
    check("%s 서버 프로세스 생존" % label, server.is_alive)
    check("%s fd 수 복구 (전 %d → 후 %d)" % (label, before["fd"], after["fd"]),
          after["fd"] <= before["fd"] + 2)
    if stable_with:
        check("%s 같은 부하를 반복해도 메모리가 늘지 않음 (1회 후 %d KB → 2회 후 %d KB, 시작 %d KB)"
              % (label, stable_with["rss"], after["rss"], before["rss"]), after["rss"] <= limit)
    else:
        check("%s 메모리 복구 (전 %d KB → 후 %d KB)" % (label, before["rss"], after["rss"]),
              after["rss"] <= limit)
    leaks = server.count_leaks()
    if leaks is not None:
        check("%s 누수 검사(leaks): %d건, %d바이트" % (label, leaks[0], leaks[1]), leaks[0] == 0)
    return after


async def read_response(reader):
    head = await reader.readuntil(b"\r\n\r\n")
    lines = head.decode("latin-1").split("\r\n")
    status = int(lines[0].split(" ")[1])
    length = 0
    for line in lines[1:]:
        if line.lower().startswith("content-length:"):
            length = int(line.split(":")[1])
    body = await reader.readexactly(length) if length else b""
    return status, body


async def fetch(reader, writer, path):
    writer.write(("GET %s HTTP/1.1\r\nHost: t\r\n\r\n" % path).encode())
    await writer.drain()
    return await read_response(reader)


async def open_connection(port, timeout):
    return await asyncio.wait_for(asyncio.open_connection(LOCALHOST, port, limit=1 << 20), timeout)


# ---------- TST-11: 동시 연결 1만 개 + 상한 초과 ----------
async def hold_and_fetch(server, count, path="/small.txt", body=b"small"):
    """count개를 동시에 연결해 붙잡은 채 한꺼번에 요청하고, 정확한 응답 수를 센다."""
    pairs = []
    errors = 0
    waves = [count // 20] * 20
    waves[-1] += count - sum(waves)
    for wave in waves:
        results = await asyncio.gather(*[open_connection(server.port, 30) for _ in range(wave)],
                                       return_exceptions=True)
        for result in results:
            if isinstance(result, Exception):
                errors += 1
            else:
                pairs.append(result)
    peak = server.fd_count()
    results = await asyncio.gather(*[fetch(r, w, path) for r, w in pairs], return_exceptions=True)
    wrong = sum(1 for r in results if r != (200, body))
    for r, w in pairs:
        w.close()
    await asyncio.sleep(0.5)
    return len(pairs), errors, wrong, peak


async def scenario_many_connections(base, count, max_connections, rounds):
    root, _ = make_files(base)
    server = Server(root, max_connections=max_connections, fd_limit=256, log_level="info")
    before = snapshot(server)
    match = re.search(r"fd 한도 (\d+)", server.log_text())
    check("TST-11 서버가 fd 한도 256에서 시작해 스스로 한도를 올림 (적용된 한도 %s)" % (match.group(1) if match else "?"),
          match is not None and int(match.group(1)) > max_connections)
    stable = None
    for index in range(rounds):
        if index > 0:
            if count >= 5000:
                print("    (클라이언트 TIME_WAIT 포트가 비도록 45초 쉼)")
                await asyncio.sleep(45)
            stable = snapshot(server)
        started = time.time()
        held, errors, wrong, peak = await hold_and_fetch(server, count)
        check("TST-11 동시 %d개 연결 성립 (연결 오류 %d)" % (count, errors), errors == 0)
        check("TST-11 %d개 연결이 동시에 요청해 모두 정확한 응답 (오류 %d)" % (held, wrong), wrong == 0)
        check("TST-11 서버 fd 수가 연결 수를 넘지 않음 (%d)" % peak, peak <= max_connections + 30 + count_extra_loop_fds())
        record("TST-11", round=index + 1, connections=held, elapsed=round(time.time() - started, 1),
               peak_fd=peak, errors=errors + wrong)
        if stable is None and rounds > 1:
            await asyncio.sleep(20)
            stable = snapshot(server)
    check_recovered("TST-11", server, before, wait_seconds=4, stable_with=stable)
    check("TST-11 서버 로그에 ERROR 없음", "[ERROR]" not in server.log_text())
    server.stop()


async def scenario_over_limit_wrk(base, max_connections, requested):
    """상한을 넘는 연결 시도: C로 만든 wrk로 requested개를 붙인다(Python 클라이언트는 이 규모에서 먼저 포화된다)."""
    root, _ = make_files(base)
    server = Server(root, max_connections=max_connections, fd_limit=256)
    before = snapshot(server)
    load = asyncio.ensure_future(measure_wrk(server.port, connections=requested, threads=4, seconds=25))
    await asyncio.sleep(12)
    peak = server.fd_count()
    check("TST-11 상한 초과(%d개 시도) 중 서버 생존, 활성 연결이 상한 이하 (fd %d ≤ %d)"
          % (requested, peak, max_connections + 30 + count_extra_loop_fds()), server.is_alive and peak <= max_connections + 30 + count_extra_loop_fds())
    result = await load
    check("TST-11 상한 초과 부하가 끝난 뒤에도 서버 생존", server.is_alive)
    reader, writer = await open_connection(server.port, 10)
    status, body = await fetch(reader, writer, "/small.txt")
    check("TST-11 연결이 줄면 accept를 다시 시작해 새 손님에게 정확한 응답", status == 200 and body == b"small")
    writer.close()
    record("TST-11-over-limit", requested=requested, max_connections=max_connections, peak_fd=peak, wrk=result)
    check_recovered("TST-11", server, before, wait_seconds=4, memory_margin_kb=70000)
    check("TST-11 서버 로그에 ERROR 없음", "[ERROR]" not in server.log_text())
    server.stop()


async def held_client(port, index, release_count, events, stats):
    """연결을 유지하며 2초마다 요청한다. 서버가 유휴 연결을 정리하므로 활동이 있어야 한다."""
    reader = writer = None
    give_up = time.time() + 120
    while reader is None and time.time() < give_up and not events["stop"].is_set():
        try:
            reader, writer = await open_connection(port, 20)
        except Exception:
            await asyncio.sleep(1)
    if reader is None:
        return
    try:
        while not events["stop"].is_set():
            if index < release_count and events["release"].is_set():
                break
            status, body = await asyncio.wait_for(fetch(reader, writer, "/small.txt"), 20)
            if status == 200 and body == b"small":
                stats["ok"].add(index)
            else:
                stats["wrong"] += 1
            await asyncio.sleep(2)
    except Exception:
        stats["dropped"] += 1
    finally:
        writer.close()


async def scenario_over_limit_small(base, max_connections, requested):
    """연결이 줄면 대기하던 연결을 다시 받는지(의미 확인). 연결을 유지하며 요청하는 클라이언트는
    Python이라 큰 규모에서 포화되므로 상한 1,500 규모로 확인한다."""
    root, _ = make_files(base)
    server = Server(root, max_connections=max_connections, fd_limit=256)
    extra = requested + max(requested // 5, 200)
    release = max(max_connections // 5, 100)
    events = {"stop": asyncio.Event(), "release": asyncio.Event()}
    stats = {"ok": set(), "wrong": 0, "dropped": 0}
    tasks = [asyncio.ensure_future(held_client(server.port, i, release, events, stats)) for i in range(extra)]
    await asyncio.sleep(14)
    peak = server.fd_count()
    served_before = len(stats["ok"])
    check("TST-11 (소규모) 상한 %d 초과 시도 중 활성 연결이 상한 이하 (fd %d ≤ %d)"
          % (max_connections, peak, max_connections + 30 + count_extra_loop_fds()), server.is_alive and peak <= max_connections + 30 + count_extra_loop_fds())
    events["release"].set()
    await asyncio.sleep(25)
    events["stop"].set()
    await asyncio.gather(*tasks)
    check("TST-11 (소규모) 연결이 줄면 대기하던 연결을 다시 accept (응답받은 손님 %d → %d, 상한 %d)"
          % (served_before, len(stats["ok"]), max_connections), len(stats["ok"]) > max_connections)
    check("TST-11 (소규모) 상한 초과 중에도 잘못된 응답 0 (잘못된 응답 %d, 끊김 %d)" % (stats["wrong"], stats["dropped"]),
          stats["wrong"] == 0 and stats["dropped"] == 0)
    server.stop()


# ---------- TST-12: 느린 손님(Slowloris) ----------
async def slow_client(port, results, index):
    try:
        reader, writer = await open_connection(port, 10)
        start = time.time()
        writer.write(b"GET / HTTP/1.1\r\nHost: x\r\nX-Slow: ")
        await writer.drain()
        while time.time() - start < 40:
            try:
                data = await asyncio.wait_for(reader.read(1), 1.0)
                if data == b"":
                    break
            except asyncio.TimeoutError:
                writer.write(b"a")
                await writer.drain()
        results[index] = time.time() - start
        writer.close()
    except Exception:
        results[index] = -1


async def scenario_slowloris(base, slow_count, seconds):
    root, _ = make_files(base)
    server = Server(root)
    before = snapshot(server)
    baseline = await measure_wrk(server.port, seconds=seconds)
    results = [None] * slow_count
    tasks = [asyncio.ensure_future(slow_client(server.port, results, i)) for i in range(slow_count)]
    await asyncio.sleep(2)
    loaded = await measure_wrk(server.port, seconds=seconds)
    await asyncio.gather(*tasks)
    judge_latency("TST-12", baseline, loaded)
    closed = [r for r in results if r is not None and r > 0]
    check("TST-12 느린 손님 %d명 모두 서버가 정리 (정리됨 %d)" % (slow_count, len(closed)), len(closed) == slow_count)
    check("TST-12 정리 시점이 첫 바이트 후 15초 전후 (%.1f~%.1f초)" % (min(closed), max(closed)),
          all(14.5 <= r <= 17.5 for r in closed))
    record("TST-12", baseline=baseline, loaded=loaded, closed_at=[round(min(closed), 1), round(max(closed), 1)])
    check_recovered("TST-12", server, before)
    server.stop()


# ---------- TST-13: keep-alive 유휴 손님 ----------
async def idle_client(port, results, index, send_request):
    try:
        reader, writer = await open_connection(port, 30)
        if send_request:
            status, _ = await fetch(reader, writer, "/small.txt")
            assert status == 200
        start = time.time()
        data = await asyncio.wait_for(reader.read(1), 30)
        results[index] = time.time() - start if data == b"" else -2
        writer.close()
    except Exception:
        results[index] = -1


async def scenario_idle(base, count):
    root, _ = make_files(base)
    server = Server(root)
    before = snapshot(server)
    results = [None] * count
    half = count // 2
    await asyncio.gather(*[idle_client(server.port, results, i, i < half) for i in range(count)])
    after_request = results[:half]
    never_sent = results[half:]
    check("TST-13 keep-alive 유휴 %d명 모두 서버가 닫음" % half, all(r is not None and r > 0 for r in after_request))
    check("TST-13 응답 후 약 5초에 정리 (%.1f~%.1f초)" % (min(after_request), max(after_request)),
          all(4.8 <= r <= 7.0 for r in after_request))
    check("TST-13 요청 없이 붙잡고 있던 %d명도 약 15초에 정리 (%.1f~%.1f초)"
          % (len(never_sent), min(never_sent), max(never_sent)),
          all(r is not None and 14.5 <= r <= 17.5 for r in never_sent))
    record("TST-13", clients=count, closed_after=[round(min(after_request), 1), round(max(after_request), 1)])
    check_recovered("TST-13", server, before)
    server.stop()


# ---------- TST-14: 100MB 파일 + 작은 요청 ----------
async def big_download(port, digest, slow, results, index):
    try:
        reader, writer = await open_connection(port, 20)
        writer.write(b"GET /big100.bin HTTP/1.1\r\nHost: t\r\n\r\n")
        await writer.drain()
        head = await reader.readuntil(b"\r\n\r\n")
        length = int(re.search(rb"Content-Length: (\d+)", head).group(1))
        sha = __import__("hashlib").sha256()
        received = 0
        while received < length:
            chunk = await reader.read(min(1 << 18, length - received))
            if not chunk:
                break
            sha.update(chunk)
            received += len(chunk)
            if slow:
                await asyncio.sleep(0.02)
        results[index] = received == length and sha.hexdigest() == digest
        writer.close()
    except Exception:
        results[index] = False


async def scenario_big_and_small(base, downloads, seconds):
    root, digest = make_files(base)
    server = Server(root)
    before = snapshot(server)
    baseline = await measure_wrk(server.port, seconds=seconds)
    results = [None] * downloads
    tasks = [asyncio.ensure_future(big_download(server.port, digest, i % 2 == 0, results, i))
             for i in range(downloads)]
    await asyncio.sleep(1)
    peak = snapshot(server)
    loaded = await measure_wrk(server.port, seconds=seconds)
    judge_latency("TST-14", baseline, loaded)
    await asyncio.gather(*tasks)
    check("TST-14 100MB 다운로드 %d개 모두 해시 일치" % downloads, all(results))
    check("TST-14 전송 중 메모리 증가가 10MB 미만 (파일은 100MB, 전 %d KB → 전송 중 %d KB)" % (before["rss"], peak["rss"]),
          peak["rss"] - before["rss"] < 10 * 1024)
    record("TST-14", baseline=baseline, loaded=loaded, rss_peak_kb=peak["rss"])
    check_recovered("TST-14", server, before)
    server.stop()


# ---------- TST-15, 16: 쪼개 보내기 / 붙여 보내기 ----------
async def split_client(port, index, results):
    try:
        reader, writer = await open_connection(port, 30)
        name = "/file_%d.txt" % (index % 10)
        raw = ("GET %s HTTP/1.1\r\nHost: t\r\n\r\n" % name).encode()
        for byte in raw:
            writer.write(bytes([byte]))
            await writer.drain()
            await asyncio.sleep(0)
        status, body = await asyncio.wait_for(read_response(reader), 30)
        results[index] = status == 200 and body == ("content-of-file-%d" % (index % 10)).encode()
        writer.close()
    except Exception:
        results[index] = False


async def pipelined_client(port, index, count, results):
    try:
        reader, writer = await open_connection(port, 30)
        order = [random.Random(index * 1000 + i).randrange(10) for i in range(count)]
        raw = b"".join(("GET /file_%d.txt HTTP/1.1\r\nHost: t\r\n\r\n" % n).encode() for n in order)
        writer.write(raw)
        await writer.drain()
        good = True
        for n in order:
            status, body = await asyncio.wait_for(read_response(reader), 30)
            good = good and status == 200 and body == ("content-of-file-%d" % n).encode()
        results[index] = good
        writer.close()
    except Exception:
        results[index] = False


async def scenario_split_and_pipeline(base, clients, depth):
    root, _ = make_files(base)
    server = Server(root)
    before = snapshot(server)
    stable = None
    for round_index in range(2):
        results = [None] * clients
        await asyncio.gather(*[split_client(server.port, i, results) for i in range(clients)])
        check("TST-15 1바이트씩 쪼갠 요청 %d개가 모두 정확한 응답 (%d회차)" % (clients, round_index + 1), all(results))
        results = [None] * clients
        await asyncio.gather(*[pipelined_client(server.port, i, depth, results) for i in range(clients)])
        check("TST-16 연결 %d개 × 붙여 보낸 요청 %d개가 순서대로 정확한 응답 (%d회차)"
              % (clients, depth, round_index + 1), all(results))
        if round_index == 0:
            await asyncio.sleep(15)
            stable = snapshot(server)
    record("TST-15/16", clients=clients, depth=depth)
    check_recovered("TST-15/16", server, before, wait_seconds=6, stable_with=stable)
    server.stop()


# ---------- TST-17: 응답 도중 대량 끊기 ----------
async def abrupt_client(port, index, mode):
    try:
        reader, writer = await open_connection(port, 20)
        if mode == 0:
            writer.write(b"GET /big100.bin HTTP/1.1\r\nHost: t\r\n\r\n")
            await writer.drain()
            await asyncio.wait_for(reader.read(100000), 10)
        elif mode == 1:
            writer.write(b"GET /big100.bin HTTP/1.1\r\nHost: t\r\n\r\n")
            await writer.drain()
        else:
            writer.write(b"GET /small.txt HTTP/1.1\r\nHost: t\r\nX-Partial: ")
            await writer.drain()
        sock = writer.get_extra_info("socket")
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
        writer.close()
        return True
    except Exception:
        return False


async def run_abrupt_round(server, clients):
    """끊기를 실제로 해낸 클라이언트 수를 돌려준다. 클라이언트가 먼저 실패하면 서버를 시험한 것이 아니다."""
    succeeded = 0
    for start in range(0, clients, 500):
        results = await asyncio.gather(*[abrupt_client(server.port, i, i % 3)
                                         for i in range(start, min(start + 500, clients))])
        succeeded += sum(1 for r in results if r)
    await asyncio.sleep(1)
    return succeeded


async def scenario_abrupt(base, clients):
    root, _ = make_files(base)
    server = Server(root)
    before = snapshot(server)
    done_first = await run_abrupt_round(server, clients)
    await asyncio.sleep(10)
    first = snapshot(server)
    done_second = await run_abrupt_round(server, clients)
    check("TST-17 끊기 %d개 × 2회가 실제로 수행됨 (1회 %d, 2회 %d, 95%% 이상이어야 함)" % (clients, done_first, done_second),
          min(done_first, done_second) >= clients * 0.95)
    reader, writer = await open_connection(server.port, 5)
    status, body = await fetch(reader, writer, "/small.txt")
    check("TST-17 대량 끊김 뒤에도 새 손님에게 정확한 응답", status == 200 and body == b"small")
    writer.close()
    await asyncio.sleep(10)
    second = snapshot(server)
    record("TST-17", clients=clients, performed=[done_first, done_second],
           memory_after_rounds_kb=[first["rss"], second["rss"]])
    check_recovered("TST-17", server, before, wait_seconds=3, stable_with=first)
    check("TST-17 서버 로그에 ERROR 없음", "[ERROR]" not in server.log_text())
    server.stop()


# ---------- TST-18: 퍼징 ----------
# 시드를 여러 개 돌려 매번 같은 입력만 보내지 않게 한다 (처음 값은 이전 실행과 같은 입력)
FUZZ_SEEDS = (20261123, 7, 20240229)
VALID = [b"GET /small.txt HTTP/1.1\r\nHost: t\r\n\r\n",
         b"POST /small.txt HTTP/1.1\r\nHost: t\r\nContent-Length: 5\r\n\r\nhello",
         b"GET /file_3.txt?a=b%20c HTTP/1.0\r\nUser-Agent: x\r\nAccept: */*\r\n\r\n"]


def make_fuzz_input(rng):
    kind = rng.randrange(6)
    if kind == 0:
        return bytes(rng.randrange(256) for _ in range(rng.randrange(1, 2000)))
    base = bytearray(rng.choice(VALID))
    if kind == 1:
        for _ in range(rng.randrange(1, 6)):
            base[rng.randrange(len(base))] = rng.randrange(256)
    elif kind == 2:
        del base[rng.randrange(len(base)):]
    elif kind == 3:
        position = rng.randrange(len(base))
        base[position:position] = bytes(rng.randrange(256) for _ in range(rng.randrange(1, 50)))
    elif kind == 4:
        base[base.find(b"\r\n"):base.find(b"\r\n")] = b"X" * rng.choice([100, 9000, 20000])
    else:
        base = bytearray(b"\r\n" * rng.randrange(1, 500))
    return bytes(base)


async def fuzz_client(port, data, results, index):
    try:
        reader, writer = await open_connection(port, 20)
        writer.write(data)
        await writer.drain()
        try:
            head = await asyncio.wait_for(reader.read(200), 1.0)
        except asyncio.TimeoutError:
            head = None
        results[index] = head
        writer.close()
    except Exception:
        results[index] = b""


async def scenario_fuzz(base, count):
    root, _ = make_files(base)
    server = Server(root)
    before = snapshot(server)
    unexpected = responded = closed_without_reply = waiting = 0
    for seed in FUZZ_SEEDS:
        rng = random.Random(seed)
        inputs = [make_fuzz_input(rng) for _ in range(count)]
        results = [None] * count
        for start in range(0, count, 200):
            await asyncio.gather(*[fuzz_client(server.port, inputs[i], results, i)
                                   for i in range(start, min(start + 200, count))])
        for head in results:
            if head is None:
                waiting += 1            # 1초 안에 응답도 종료도 없음: 덜 온 요청을 기다리는 서버의 정상 동작
            elif head == b"":
                closed_without_reply += 1   # 응답 없이 연결이 끊김(서버가 닫았거나 연결 실패)
            else:
                responded += 1
                match = re.match(rb"HTTP/1\.1 (\d{3})", head)
                code = int(match.group(1)) if match else 0
                if code not in (200, 301, 400, 403, 404, 405, 411, 413, 414, 417, 431, 501, 505):
                    unexpected += 1
    total = count * len(FUZZ_SEEDS)
    check("TST-18 이상한 요청 %d개(시드 %d개): 응답한 %d개 중 예상 밖 코드 %d" % (total, len(FUZZ_SEEDS), responded, unexpected),
          unexpected == 0)
    check("TST-18 응답 없이 연결만 끊긴 입력이 전체의 5%% 미만 (%d / %d, 기다림 %d)"
          % (closed_without_reply, total, waiting), closed_without_reply * 20 < total)
    check("TST-18 서버 생존", server.is_alive)
    time.sleep(17)
    reader, writer = await open_connection(server.port, 5)
    status, body = await fetch(reader, writer, "/small.txt")
    check("TST-18 퍼징 뒤 새 손님에게 정확한 응답", status == 200 and body == b"small")
    writer.close()
    record("TST-18", inputs=total, seeds=list(FUZZ_SEEDS), responded=responded,
           closed_without_reply=closed_without_reply, waiting=waiting, unexpected=unexpected)
    check_recovered("TST-18", server, before, wait_seconds=1)
    check("TST-18 서버 로그에 ERROR 없음", "[ERROR]" not in server.log_text())
    server.stop()


# ---------- TST-19: 백엔드 느림·죽음 + 동시 부하 ----------
async def proxy_hammer(port, path, results, index, rounds):
    for _ in range(rounds):
        try:
            reader, writer = await open_connection(port, 30)
            status, _ = await asyncio.wait_for(fetch(reader, writer, path), 30)
            results.append(status)
            writer.close()
        except Exception:
            results.append(-1)


async def scenario_backend_trouble(base, seconds):
    root, _ = make_files(base)
    python_backend = PythonBackend()
    dead_port = find_free_port()
    config = os.path.join(base, "trouble.conf")
    with open(config, "w") as handle:
        handle.write("route /dead 127.0.0.1:%d\n" % dead_port)
        handle.write("route /py 127.0.0.1:%d\n" % python_backend.port)
        handle.write("connect_timeout 1\nresponse_timeout 3\n")
    server = Server(root, config=config)
    before = snapshot(server)
    baseline = await measure_wrk(server.port, seconds=seconds)
    dead_results = []
    slow_results = []
    hammers = [asyncio.ensure_future(proxy_hammer(server.port, "/dead/x", dead_results, i, 60)) for i in range(20)]
    hammers += [asyncio.ensure_future(proxy_hammer(server.port, "/py/slow?seconds=30", slow_results, i, 1))
                for i in range(100)]
    await asyncio.sleep(1)
    loaded = await measure_wrk(server.port, seconds=seconds)
    await asyncio.gather(*hammers)
    judge_latency("TST-19", baseline, loaded)
    check("TST-19 죽은 백엔드 요청 %d개가 모두 502" % len(dead_results), all(s == 502 for s in dead_results))
    check("TST-19 느린 백엔드 요청 %d개가 모두 504" % len(slow_results), all(s == 504 for s in slow_results))
    record("TST-19", baseline=baseline, loaded=loaded, dead=len(dead_results), slow=len(slow_results))
    check_recovered("TST-19", server, before, wait_seconds=2)
    server.stop()
    python_backend.stop()


# ---------- TST-20: 장시간 실행 ----------
def rss_slope_kb_per_minute(samples):
    """시간에 따른 메모리의 최소제곱 기울기. 앞뒤 평균만 비교하면 느리게 새는 누수를 놓치므로 함께 본다."""
    xs = [s["minute"] for s in samples]
    ys = [s["rss"] for s in samples]
    mean_x = sum(xs) / len(xs)
    mean_y = sum(ys) / len(ys)
    spread = sum((x - mean_x) ** 2 for x in xs)
    if spread == 0:
        return 0.0
    return sum((x - mean_x) * (y - mean_y) for x, y in zip(xs, ys)) / spread


async def scenario_long_run(base, minutes):
    root, digest = make_files(base)
    python_backend = PythonBackend()
    config = os.path.join(base, "long.conf")
    with open(config, "w") as handle:
        handle.write("route /py %s\n" % ("127.0.0.1:%d" % python_backend.port))
        handle.write("connect_timeout 1\nresponse_timeout 2\n")
    server = Server(root, config=config)
    samples = []
    end = time.time() + minutes * 60
    started = time.time()
    rng = random.Random(7)
    while time.time() < end:
        load = asyncio.ensure_future(measure_wrk(server.port, connections=50, threads=2, seconds=20))
        mix = []
        for round_index in range(6):
            kind = round_index % 3
            if kind == 0:
                mix.append(asyncio.ensure_future(scenario_chaos_round(server.port, rng)))
            else:
                mix.append(asyncio.ensure_future(proxy_hammer(server.port, "/py/echo?x=1", [], 0, 20)))
        await asyncio.gather(load, *mix)
        sample = dict(minute=round((time.time() - started) / 60, 1), **snapshot(server))
        samples.append(sample)
        print("    장시간 %(minute).1f분: fd=%(fd)d rss=%(rss)d KB" % sample, flush=True)
    check("TST-20 서버 생존 (%d분)" % minutes, server.is_alive)
    warm = samples[len(samples) // 5:]
    first = warm[:max(len(warm) // 3, 1)]
    last = warm[-max(len(warm) // 3, 1):]
    rss_first = sum(s["rss"] for s in first) / len(first)
    rss_last = sum(s["rss"] for s in last) / len(last)
    check("TST-20 메모리가 계속 늘지 않음 (앞 %.0f KB → 뒤 %.0f KB, 허용 앞 × 1.15 + 1MB)" % (rss_first, rss_last),
          rss_last <= rss_first * 1.15 + 1024)
    if minutes >= 10:
        slope = rss_slope_kb_per_minute(warm)
        check("TST-20 메모리 증가 기울기가 분당 50KB 이하 (%.1f KB/분, 30분이면 %.0f KB)" % (slope, slope * 30),
              slope <= 50)
    else:
        print("  건너뜀  TST-20 기울기 검사는 10분 이상 실행에서만 한다 (이번 %.1f분)" % minutes)
    check("TST-20 fd가 계속 늘지 않음 (앞 %d → 뒤 %d)" % (first[0]["fd"], last[-1]["fd"]),
          last[-1]["fd"] <= first[0]["fd"] + 5)
    record("TST-20", minutes=minutes, samples=samples)
    server.stop()
    python_backend.stop()


async def scenario_chaos_round(port, rng):
    tasks = [abrupt_client(port, i, i % 3) for i in range(200)]
    tasks += [fuzz_client(port, make_fuzz_input(rng), [None], 0) for _ in range(200)]
    await asyncio.gather(*tasks)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--quick", action="store_true", help="규모를 줄인 빠른 실행")
    parser.add_argument("--only", default="", help="실행할 번호 목록, 예: 11,12")
    parser.add_argument("--long-minutes", type=float, default=0)
    arguments = parser.parse_args()
    wanted = set(arguments.only.split(",")) if arguments.only else set()

    def selected(number):
        return not wanted or number in wanted

    limit = raise_client_fd_limit()
    print("클라이언트 fd 한도:", limit)
    quick = arguments.quick
    base = tempfile.mkdtemp(prefix="ws_stress_")
    loop = asyncio.new_event_loop()
    try:
        if selected("11"):
            print("[TST-11 동시 연결]")
            if quick:
                loop.run_until_complete(scenario_many_connections(base, 1400, 1500, 2))
                loop.run_until_complete(scenario_over_limit_wrk(base, 1500, 1800))
                loop.run_until_complete(scenario_over_limit_small(base, 1500, 1400))
            else:
                loop.run_until_complete(scenario_many_connections(base, 10000, 10000, 2))
                print("    (클라이언트 TIME_WAIT 포트가 비도록 45초 쉼)")
                time.sleep(45)
                loop.run_until_complete(scenario_over_limit_wrk(base, 10000, 12000))
                loop.run_until_complete(scenario_over_limit_small(base, 1500, 1400))
        if selected("12"):
            print("[TST-12 느린 손님]")
            loop.run_until_complete(scenario_slowloris(base, 100 if quick else 400, 6 if quick else 10))
        if selected("13"):
            print("[TST-13 keep-alive 유휴]")
            loop.run_until_complete(scenario_idle(base, 600 if quick else 4000))
        if selected("14"):
            print("[TST-14 큰 파일 + 작은 요청]")
            loop.run_until_complete(scenario_big_and_small(base, 4 if quick else 10, 6 if quick else 10))
        if selected("15"):
            print("[TST-15/16 쪼개기·붙이기]")
            loop.run_until_complete(scenario_split_and_pipeline(base, 200 if quick else 1000, 30 if quick else 100))
        if selected("17"):
            print("[TST-17 응답 도중 대량 끊기]")
            loop.run_until_complete(scenario_abrupt(base, 900 if quick else 6000))
        if selected("18"):
            print("[TST-18 퍼징]")
            loop.run_until_complete(scenario_fuzz(base, 600 if quick else 5000))
        if selected("19"):
            print("[TST-19 백엔드 느림·죽음]")
            loop.run_until_complete(scenario_backend_trouble(base, 6 if quick else 10))
        if arguments.long_minutes > 0 and selected("20"):
            print("[TST-20 장시간 실행]")
            loop.run_until_complete(scenario_long_run(base, arguments.long_minutes))
    finally:
        shutil.rmtree(base, ignore_errors=True)
        results_dir = os.path.join(os.path.dirname(os.path.abspath(__file__)), "results")
        os.makedirs(results_dir, exist_ok=True)
        name = "stress_quick.json" if quick else "stress_full.json"
        if arguments.long_minutes > 0 and wanted == {"20"}:
            name = "stress_long.json"
        with open(os.path.join(results_dir, name), "w") as handle:
            json.dump(RESULTS, handle, indent=1, ensure_ascii=False)
    return checker.finish()


if __name__ == "__main__":
    sys.exit(main())
