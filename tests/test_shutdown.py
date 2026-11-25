"""6단계 통합 테스트 (docs/step6-shutdown.md 3번 SD-1~7)

사용: python3 tests/test_shutdown.py <서버 실행 파일> [--asan]

--asan이면 서버가 ASan(누수 검사 켬) 빌드라고 보고 SD-7을 확인한다.
서버마다 새로 띄우고 SIGINT·SIGTERM을 직접 보낸다.
리눅스 전용 (/proc, 신호, D5).
"""

import hashlib
import json
import os
import shutil
import signal
import socket
import sys
import tempfile
import threading
import time

TESTS_DIRECTORY = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, TESTS_DIRECTORY)

import test_event_loop as base  # noqa: E402
import test_proxy as proxy  # noqa: E402
from test_event_loop import (  # noqa: E402
    Client, Server, check, check_elapsed, get, run_test, results)

# ── 서버 설정 (E5: 짧은 값) ──

# (SD-1~5) /api/slow?ms=2000(SD-3)이 마감 전에 끝나야 한다
SHUTDOWN_TIMEOUT = 3.0
# (SD-6)
SHORT_SHUTDOWN_TIMEOUT = 1.0
# "바로 종료"의 상한
QUICK_EXIT = 0.5
# 종료를 기다리는 상한 (이보다 오래 걸리면 실패)
EXIT_WAIT = 8.0

BIG_FILE_SIZE = 8 * 1024 * 1024


class ShutdownRoot:
    def __init__(self):
        self.base = tempfile.mkdtemp(prefix="step6-")
        self.root = os.path.join(self.base, "www")
        os.mkdir(self.root)
        self.add("a.html", b"<html><body>a</body></html>\n")
        self.big = os.urandom(BIG_FILE_SIZE)
        self.add("big.bin", self.big)

    def add(self, name, content):
        with open(os.path.join(self.root, name), "wb") as file:
            file.write(content)

    def remove(self):
        shutil.rmtree(self.base)


class Environment:
    pass


def start_server(env, name, shutdown_timeout=SHUTDOWN_TIMEOUT,
                 read_timeout="5s"):
    options = base.SHORT_TIMEOUT_OPTIONS + [
        "count_log on",
        "backend_read_timeout " + read_timeout,
        "shutdown_timeout %dms" % int(shutdown_timeout * 1000),
        "route /api round_robin 127.0.0.1:%d" % env.backend.port,
    ]
    server = Server(env.binary, env.root.root, env.root.base, name, options)
    env.servers.append(server)
    time.sleep(0.3)
    return server


def send_signal(server, number):
    server.process.send_signal(number)
    return time.monotonic()


def wait_exit(server):
    """반환: (종료 코드, 신호 뒤 걸린 초는 호출자가 잰다)"""
    try:
        return server.process.wait(timeout=EXIT_WAIT)
    except Exception:
        raise base.CheckFailed("server did not exit in %.0fs" % EXIT_WAIT)


def check_done(server, forced):
    stderr = server.read_stderr()
    check("shutdown: start (active " in stderr, "no shutdown start line")
    check("shutdown: done (forced %d)" % forced in stderr,
          "no 'done (forced %d)' line:%s" % (forced, tail(stderr)))


def tail(text):
    return "".join("\n    " + line for line in text.splitlines()[-5:])


def start_slow_request(server, milliseconds):
    """반환: (스레드, 결과 dict). 결과에 response 또는 error가 들어간다"""
    outcome = {}

    def run():
        try:
            outcome["response"] = get(server.port,
                                      "/api/slow?ms=%d" % milliseconds)
        except Exception as error:  # 강제 종료면 연결이 끊긴다
            outcome["error"] = error
    thread = threading.Thread(target=run)
    thread.start()
    return thread, outcome


# ── SD. 정상 종료 (E1~E7) ──

def test_sd1_idle_server(env):
    server = start_server(env, "sd1")
    started = send_signal(server, signal.SIGINT)
    code = wait_exit(server)
    elapsed = time.monotonic() - started
    check(code == 0, "exit code %r" % code)
    check(elapsed < QUICK_EXIT, "exit after %.2fs" % elapsed)
    check_done(server, 0)


def test_sd2_finish_download(env):
    server = start_server(env, "sd2")
    client = Client(server.port, proxy.SMALL_RECEIVE_BUFFER)
    client.send(base.build_request("/big.bin"))
    response = client.read_head_only()
    check(response.status == 200, "status %d" % response.status)
    while len(client.buffer) < 64 * 1024:
        client.receive_more()
    send_signal(server, signal.SIGTERM)
    length = int(response.header("Content-Length"))
    while len(client.buffer) < length:
        client.receive_more()
    body = client.buffer[:length]
    client.close()
    check(hashlib.sha256(body).digest() == hashlib.sha256(env.root.big)
          .digest(), "body differs")
    code = wait_exit(server)
    check(code == 0, "exit code %r" % code)
    check_done(server, 0)


def test_sd3_finish_proxy(env):
    server = start_server(env, "sd3")
    thread, outcome = start_slow_request(server, 2000)
    time.sleep(0.3)
    send_signal(server, signal.SIGINT)
    thread.join()
    response = outcome.get("response")
    check(response is not None, "no response: %r" % outcome.get("error"))
    check(response.status == 200, "status %d" % response.status)
    code = wait_exit(server)
    check(code == 0, "exit code %r" % code)
    active = proxy.find_last_active(server).get(env.backend.port)
    check(active == 0, "backend active=%r" % active)
    check_done(server, 0)


def test_sd4_idle_keepalive_closed(env):
    server = start_server(env, "sd4")
    client = Client(server.port)
    client.send(base.build_request("/a.html"))
    response = client.read_response()
    check(response.status == 200, "status %d" % response.status)
    started = send_signal(server, signal.SIGINT)
    _, how = client.read_until_close(QUICK_EXIT)
    client.close()
    check(how in ("eof", "reset"), "idle connection not closed (%s)" % how)
    code = wait_exit(server)
    elapsed = time.monotonic() - started
    check(code == 0, "exit code %r" % code)
    check(elapsed < QUICK_EXIT, "exit after %.2fs" % elapsed)
    check_done(server, 0)


def test_sd5_new_connect_refused(env):
    server = start_server(env, "sd5")
    thread, _ = start_slow_request(server, 1000)
    time.sleep(0.3)
    send_signal(server, signal.SIGINT)
    time.sleep(0.1)
    probe = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    probe.settimeout(1.0)
    try:
        probe.connect(("127.0.0.1", server.port))
        refused = False
    except ConnectionRefusedError:
        refused = True
    finally:
        probe.close()
    thread.join()
    wait_exit(server)
    check(refused, "connect after shutdown start was accepted")


def test_sd6_deadline_and_second_signal(env):
    server = start_server(env, "sd6a", SHORT_SHUTDOWN_TIMEOUT, "30s")
    thread, _ = start_slow_request(server, 20000)
    time.sleep(0.3)
    started = send_signal(server, signal.SIGINT)
    code = wait_exit(server)
    elapsed = time.monotonic() - started
    thread.join()
    check(code == 0, "deadline: exit code %r" % code)
    check_elapsed(elapsed, SHORT_SHUTDOWN_TIMEOUT, "deadline exit")
    check_done(server, 1)

    server = start_server(env, "sd6b", SHORT_SHUTDOWN_TIMEOUT, "30s")
    thread, _ = start_slow_request(server, 20000)
    time.sleep(0.3)
    send_signal(server, signal.SIGINT)
    time.sleep(0.1)
    started = send_signal(server, signal.SIGINT)
    code = wait_exit(server)
    elapsed = time.monotonic() - started
    thread.join()
    check(code == 0, "second signal: exit code %r" % code)
    check(elapsed < QUICK_EXIT, "second signal: exit after %.2fs" % elapsed)
    check_done(server, 1)


def test_sd7_sanitizer_clean(env):
    for server in env.servers:
        stderr = server.read_stderr()
        check("AddressSanitizer" not in stderr,
              "AddressSanitizer report (%s)" % server.stdout_path)
        check("LeakSanitizer" not in stderr,
              "LeakSanitizer report (%s)" % server.stdout_path)
        check(server.process.returncode == 0,
              "exit code %r (%s)" % (server.process.returncode,
                                     server.stdout_path))


SD_TESTS = [
    ("SD-1", test_sd1_idle_server),
    ("SD-2", test_sd2_finish_download),
    ("SD-3", test_sd3_finish_proxy),
    ("SD-4", test_sd4_idle_keepalive_closed),
    ("SD-5", test_sd5_new_connect_refused),
    ("SD-6", test_sd6_deadline_and_second_signal),
]


def main():
    if len(sys.argv) < 2:
        print("usage: %s <server binary> [--asan]" % sys.argv[0],
              file=sys.stderr)
        return 2
    env = Environment()
    env.binary = os.path.abspath(sys.argv[1])
    env.is_asan = "--asan" in sys.argv[2:]
    env.root = ShutdownRoot()
    env.backend = proxy.Backend()
    env.servers = []
    try:
        env.backend.start()
        for test_id, function in SD_TESTS:
            run_test(test_id, function, env)
        if env.is_asan:
            run_test("SD-7", test_sd7_sanitizer_clean, env)
    finally:
        for server in env.servers:
            server.stop()
        env.backend.stop()
        env.root.remove()
    print("통과 %d, 실패 %d" % (results["pass"], results["fail"]))
    if results["failed_ids"]:
        print("실패한 ID: " + ", ".join(results["failed_ids"]))
    if not env.is_asan:
        print("SD-7은 ASan 빌드(make itest-asan)에서 확인")
    return 1 if results["fail"] else 0


if __name__ == "__main__":
    sys.exit(main())
