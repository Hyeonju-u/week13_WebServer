"""4단계 통합 테스트 (docs/step4-proxy.md 6번 HD·EX·RS·FL·LB·CL)

사용: python3 tests/test_proxy.py <서버 실행 파일>

정상 응답은 backend/backend.py(G16)를 띄워서 쓰고, 이상한 응답은 이 파일
안의 가짜 소켓 백엔드로 만든다. 손님·서버 띄우기 도우미는 3단계 테스트
(test_event_loop.py)의 것을 그대로 쓴다. 타임아웃은 짧은 값으로 돌린다.
CL-6은 3단계 테스트 스크립트를 같은 서버 실행 파일로 돌려서 본다.
리눅스 전용 (/proc, SIGSTOP, D5).
"""

import json
import os
import re
import shutil
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time

TESTS_DIRECTORY = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, TESTS_DIRECTORY)

import test_event_loop as base  # noqa: E402
from test_event_loop import (  # noqa: E402
    Client, Server, check, check_closed, check_elapsed, get, run_test,
    results)

BACKEND_SCRIPT = os.path.join(os.path.dirname(TESTS_DIRECTORY), "backend",
                              "backend.py")
STEP3_TEST_SCRIPT = os.path.join(TESTS_DIRECTORY, "test_event_loop.py")

# ── 서버 설정 (G11, G13, G15: 짧은 값) ──

BACKEND_CONNECT_TIMEOUT = 0.5
BACKEND_READ_TIMEOUT = 1.0
BACKEND_FAIL_TIMEOUT = 1.0
# (LB-6) 두 번째 요청까지 빠짐 표시가 남아 있어야 한다
LONG_FAIL_TIMEOUT = "5000ms"

PROXY_SETTINGS = {
    "header_timeout": "1000ms",
    "body_timeout": "1000ms",
    "send_timeout": "1000ms",
    "keepalive_timeout": "1000ms",
    "linger_quiet": "300ms",
    "linger_total": "1500ms",
    "backend_connect_timeout": "500ms",
    "backend_send_timeout": "1000ms",
    "backend_read_timeout": "1000ms",
    "backend_fail_timeout": "1000ms",
    "count_log": "on",
}

# (FL-5) 백엔드 소켓 버퍼(커널 기본 최대 4MB)보다 큰 본문
STUCK_BODY_SIZE = 8 * 1024 * 1024
STUCK_MAX_BODY = "16M"
# (FL-5, L42) 안 읽어 가는 가짜 백엔드의 받기 버퍼
SMALL_RECEIVE_BUFFER = 4096

# (RS-7) max_backend_response 기본 8MB보다 큰 9MB
OVERSIZED_BODY_SIZE = 9 * 1024 * 1024

# (CL-2, G19) (64 − (16 + 1 × 2)) ÷ 2
FD_LIMIT_FOR_CL2 = 64
EXPECTED_MAX_CONNECTIONS = 23
CL2_CLIENT_COUNT = 40
CL2_SLOW_MS = 2000

ADDRESS = r'(\d{1,3}(?:\.\d{1,3}){3})'

# (G18, G39) 3단계 줄 + " -> IP:포트[ retry=N] (backend Nms)[ 원인]"
PROXY_LOG_PATTERN = re.compile(
    r'^\[\d{2}/[A-Z][a-z]{2}/\d{4}:\d{2}:\d{2}:\d{2} \+0000\] '
    + ADDRESS + r':(\d+) "([^"]*)" (\d{3}|-) (\d+) (\d+)'
    r'(?: (close|reset|timeout))?'
    r' -> ' + ADDRESS + r':(\d+)(?: retry=(\d+))? \(backend (\d+)ms\)'
    r'(?: (refused|timeout|reset|invalid|chunked|too_large))?$')
LOG_REQUEST = 3
LOG_STATUS = 4
LOG_BACKEND_PORT = 9
LOG_RETRY = 10
LOG_CAUSE = 12

# (LB-8, CL-1) count_log on일 때 처리 중 수가 바뀔 때마다 한 줄
ACTIVE_LOG_PATTERN = re.compile(
    r'^backend ' + ADDRESS + r':(\d+) active=(\d+)$')


# ── 정적 루트 (프록시 테스트는 파일 하나면 됨) ──

class ProxyRoot:
    def __init__(self):
        self.base = tempfile.mkdtemp(prefix="step4-")
        self.root = os.path.join(self.base, "www")
        os.mkdir(self.root)
        with open(os.path.join(self.root, "a.html"), "wb") as file:
            file.write(b"<html><body>a</body></html>\n")

    def remove(self):
        shutil.rmtree(self.base)


def make_options(routes, **overrides):
    settings = dict(PROXY_SETTINGS)
    settings.update(overrides)
    lines = ["%s %s" % (key, value) for key, value in settings.items()]
    return lines + ["route " + route for route in routes]


def backend_list(*ports):
    return " ".join("127.0.0.1:%d" % port for port in ports)


# ── 백엔드 (G16) ──

class Backend:
    """backend/backend.py 프로세스 하나"""

    def __init__(self, port=None):
        self.port = port if port is not None else base.pick_free_port()
        self.process = None

    def start(self):
        self.process = subprocess.Popen(
            [sys.executable, BACKEND_SCRIPT, str(self.port)],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        base.wait_until_listening(self.port)

    def pause(self):
        os.kill(self.process.pid, signal.SIGSTOP)

    def resume(self):
        os.kill(self.process.pid, signal.SIGCONT)

    def stop(self):
        if self.process is None or self.process.poll() is not None:
            return
        self.process.kill()
        self.process.wait()


class FakeBackend:
    """(G16) 깨진 응답·중간 끊김·안 읽어 감을 바이트 단위로 만드는 백엔드.
    연결마다 스레드 하나가 behavior(fake, conn)를 부른다"""

    def __init__(self, behavior=None, receive_buffer=None):
        self.listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        if receive_buffer is not None:
            # listen 전에 정해야 받은 연결에 물려진다
            self.listener.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF,
                                     receive_buffer)
        self.listener.bind(("127.0.0.1", 0))
        self.listener.listen(64)
        self.listener.settimeout(0.1)
        self.port = self.listener.getsockname()[1]
        self.behavior = behavior
        self.lock = threading.Lock()
        self.accepted = 0
        self.requests = []
        self.running = True
        self.thread = threading.Thread(target=self.accept_loop, daemon=True)
        self.thread.start()

    def accept_loop(self):
        while self.running:
            try:
                conn, _ = self.listener.accept()
            except socket.timeout:
                continue
            except OSError:
                return
            with self.lock:
                self.accepted += 1
            threading.Thread(target=self.serve, args=(conn,),
                             daemon=True).start()

    def serve(self, conn):
        try:
            self.behavior(self, conn)
        except OSError:
            pass
        finally:
            try:
                conn.close()
            except OSError:
                pass

    def read_request(self, conn):
        """헤더 + Content-Length만큼 읽고 원문을 기록한다"""
        conn.settimeout(5.0)
        data = b""
        while b"\r\n\r\n" not in data:
            chunk = conn.recv(65536)
            if chunk == b"":
                break
            data += chunk
        head, _, body = data.partition(b"\r\n\r\n")
        length = find_content_length(head)
        while len(body) < length:
            chunk = conn.recv(65536)
            if chunk == b"":
                break
            body += chunk
        request = head + b"\r\n\r\n" + body
        with self.lock:
            self.requests.append(request)
        return request

    def count_requests(self):
        with self.lock:
            return len(self.requests)

    def stop(self):
        self.running = False
        self.listener.close()


def find_content_length(head):
    for line in head.split(b"\r\n")[1:]:
        name, _, value = line.partition(b":")
        if name.strip().lower() == b"content-length":
            return int(value.strip())
    return 0


def respond_with(raw, hold=0.0):
    """요청을 다 읽은 뒤 raw를 보내고 hold초 있다가 닫는다"""
    def behavior(fake, conn):
        fake.read_request(conn)
        conn.sendall(raw)
        if hold > 0:
            time.sleep(hold)
    return behavior


def respond_then_reset(raw):
    def behavior(fake, conn):
        fake.read_request(conn)
        conn.sendall(raw)
        time.sleep(0.05)
        conn.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER,
                        struct.pack("ii", 1, 0))
    return behavior


def respond_without_length(size):
    """길이 없이 size바이트를 보내고 닫는다. 서버가 먼저 끊으면 멈춘다"""
    def behavior(fake, conn):
        fake.read_request(conn)
        conn.sendall(b"HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\n\r\n")
        piece = b"x" * 65536
        sent = 0
        while sent < size:
            conn.sendall(piece)
            sent += len(piece)
    return behavior


def hold_without_reading(seconds):
    def behavior(fake, conn):
        time.sleep(seconds)
    return behavior


def read_then_hold(seconds):
    def behavior(fake, conn):
        fake.read_request(conn)
        time.sleep(seconds)
    return behavior


class FullListener:
    """(FL-2) backlog 1, accept 안 함. 대기열을 미리 채워 SYN이 버려지게"""

    def __init__(self):
        self.listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.listener.bind(("127.0.0.1", 0))
        self.listener.listen(1)
        self.port = self.listener.getsockname()[1]
        self.fillers = []
        for _ in range(4):
            filler = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            filler.setblocking(False)
            filler.connect_ex(("127.0.0.1", self.port))
            self.fillers.append(filler)
        time.sleep(0.2)

    def stop(self):
        for filler in self.fillers:
            filler.close()
        self.listener.close()


# ── 손님 도우미 ──

def fetch_hello(server):
    response = get(server.port, "/api/hello")
    check(response.status == 200, "hello status %d" % response.status)
    return json.loads(response.body)["port"]


def fetch_headers(server, extra_lines, host="localhost"):
    lines = ["GET /api/headers HTTP/1.1", "Host: " + host] + list(extra_lines)
    client = Client(server.port)
    client.send(("\r\n".join(lines) + "\r\n\r\n").encode("latin-1"))
    response = client.read_response()
    client.close()
    check(response.status == 200, "headers status %d" % response.status)
    return json.loads(response.body)


def find_header_values(info, name):
    return [value for key, value in info["headers"]
            if key.lower() == name.lower()]


def post_with_expect(port, path, body):
    """curl처럼 헤더를 먼저 보내고 100을 기다린 뒤 본문을 보낸다.
    반환: (중간 응답, 최종 응답, 걸린 초)"""
    client = Client(port)
    head = ("POST %s HTTP/1.1\r\nHost: localhost\r\n"
            "Content-Length: %d\r\nExpect: 100-continue\r\n\r\n"
            % (path, len(body))).encode("latin-1")
    started = time.monotonic()
    client.send(head)
    interim = client.read_response(2.0)
    client.send(body)
    final = client.read_response()
    elapsed = time.monotonic() - started
    client.close()
    return interim, final, elapsed


def get_timed(server, path):
    started = time.monotonic()
    response = get(server.port, path)
    return response, time.monotonic() - started


def find_proxy_lines(server):
    lines = [PROXY_LOG_PATTERN.match(line)
             for line in server.read_stdout().splitlines()]
    return [match for match in lines if match is not None]


def find_last_active(server):
    """반환: {백엔드 포트: 마지막 처리 중 수}"""
    last = {}
    for line in server.read_stderr().splitlines():
        match = ACTIVE_LOG_PATTERN.match(line)
        if match is not None:
            last[int(match.group(2))] = int(match.group(3))
    return last


# ── HD. 백엔드가 받는 요청 (G4~G6) ──

def test_hd1_hop_by_hop_removed(env):
    info = fetch_headers(env.single, [
        "Connection: keep-alive, foo", "Foo: 1", "Keep-Alive: timeout=5",
        "Upgrade: websocket"])
    for name in ("Foo", "Keep-Alive", "Upgrade"):
        check(find_header_values(info, name) == [],
              "%s reached backend" % name)
    connection = find_header_values(info, "Connection")
    check(connection == ["close"], "Connection %r" % connection)


def test_hd2_host_kept(env):
    info = fetch_headers(env.single, [], host="example.test:8080")
    host = find_header_values(info, "Host")
    check(host == ["example.test:8080"], "Host %r" % host)


def test_hd3_forwarded_for(env):
    cases = [
        ([], ["127.0.0.1"]),
        (["X-Forwarded-For: 1.2.3.4"], ["1.2.3.4, 127.0.0.1"]),
        (["X-Forwarded-For: 1.2.3.4", "X-Forwarded-For: 5.6.7.8"],
         ["1.2.3.4, 5.6.7.8, 127.0.0.1"]),
    ]
    for extra, expected in cases:
        info = fetch_headers(env.single, extra)
        values = find_header_values(info, "X-Forwarded-For")
        check(values == expected, "%r -> %r" % (extra, values))


def test_hd4_raw_path_http10(env):
    # backend.py는 /api/headers 외 경로에 404라 원문 요청 줄은 가짜 백엔드로 본다
    env.fake.behavior = respond_with(
        b"HTTP/1.0 200 OK\r\nContent-Length: 2\r\n\r\nok")
    response = get(env.fake_server.port, "/api/%7Ea?x=1")
    check(response.status == 200, "status %d" % response.status)
    request_line = env.fake.requests[-1].split(b"\r\n")[0]
    check(request_line == b"GET /api/%7Ea?x=1 HTTP/1.0",
          "request line %r" % request_line)


def test_hd5_expect_removed(env):
    env.fake.behavior = respond_with(
        b"HTTP/1.0 200 OK\r\nContent-Length: 2\r\n\r\nok")
    interim, final, _ = post_with_expect(env.fake_server.port, "/api/x",
                                         b"data")
    check(interim.status == 100, "interim %d" % interim.status)
    check(final.status == 200, "status %d" % final.status)
    head = env.fake.requests[-1].split(b"\r\n\r\n")[0].lower()
    check(b"\r\nexpect:" not in head, "Expect reached backend")


# ── EX. 100 Continue (G7) ──

def test_ex1_continue_then_echo(env):
    body = os.urandom(10 * 1024)
    interim, final, _ = post_with_expect(env.single.port, "/api/echo", body)
    check(interim.status == 100, "interim %d" % interim.status)
    check(final.status == 200, "status %d" % final.status)
    check(final.body == body, "echo body differs")


def test_ex2_too_large_no_continue(env):
    client = Client(env.single.port)
    client.send(b"POST /api/echo HTTP/1.1\r\nHost: localhost\r\n"
                b"Content-Length: 2000000\r\nExpect: 100-continue\r\n\r\n")
    response = client.read_response()
    check(response.status == 413, "status %d (100 first?)" % response.status)
    check(response.header("Connection") == "close",
          "Connection %r" % response.header("Connection"))
    _, how = client.read_until_close(base.LINGER_TOTAL + base.LATE_MARGIN)
    check_closed(how)
    client.close()


def test_ex3_no_one_second_wait(env):
    _, final, elapsed = post_with_expect(env.single.port, "/api/echo",
                                         b"x" * 2048)
    check(final.status == 200, "status %d" % final.status)
    check(elapsed < 0.5, "Expect POST took %.2fs" % elapsed)


# ── RS. 백엔드 응답 (G8~G10, 가짜 백엔드) ──

def test_rs1_length_response_keepalive(env):
    env.fake.behavior = respond_with(
        b"HTTP/1.0 200 OK\r\nContent-Length: 5\r\n\r\nhello")
    client = Client(env.fake_server.port)
    for _ in range(2):
        client.send(base.build_request("/api/x"))
        response = client.read_response()
        check(response.status == 200, "status %d" % response.status)
        check(response.body == b"hello", "body %r" % response.body)
    client.close()


def test_rs2_close_delimited_gets_length(env):
    env.fake.behavior = respond_with(
        b"HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\n\r\nabcdef")
    response = get(env.fake_server.port, "/api/x")
    check(response.status == 200, "status %d" % response.status)
    check(response.header("Content-Length") == "6",
          "Content-Length %r" % response.header("Content-Length"))
    check(response.body == b"abcdef", "body %r" % response.body)


def test_rs3_short_body_502(env):
    env.fake.behavior = respond_with(
        b"HTTP/1.0 200 OK\r\nContent-Length: 100\r\n\r\n" + b"x" * 50)
    response = get(env.fake_server.port, "/api/x")
    check(response.status == 502, "status %d" % response.status)


def test_rs4_chunked_502(env):
    env.fake.behavior = respond_with(
        b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
        b"5\r\nhello\r\n0\r\n\r\n")
    response = get(env.fake_server.port, "/api/x")
    check(response.status == 502, "status %d" % response.status)


def test_rs5_skip_continue(env):
    env.fake.behavior = respond_with(
        b"HTTP/1.1 100 Continue\r\n\r\n"
        b"HTTP/1.0 200 OK\r\nContent-Length: 2\r\n\r\nok")
    response = get(env.fake_server.port, "/api/x")
    check(response.status == 200, "status %d" % response.status)
    check(response.body == b"ok", "body %r" % response.body)


def test_rs6_no_content_keepalive(env):
    env.fake.behavior = respond_with(b"HTTP/1.0 204 No Content\r\n\r\n")
    client = Client(env.fake_server.port)
    for _ in range(2):
        client.send(base.build_request("/api/x"))
        response = client.read_response()
        check(response.status == 204, "status %d" % response.status)
    client.close()


def test_rs7_oversized_502(env):
    env.fake.behavior = respond_with(
        b"HTTP/1.0 200 OK\r\nContent-Length: %d\r\n\r\n"
        % OVERSIZED_BODY_SIZE, hold=3.0)
    response, elapsed = get_timed(env.fake_server, "/api/x")
    check(response.status == 502, "length: status %d" % response.status)
    check(elapsed < BACKEND_READ_TIMEOUT * 0.5,
          "length: 502 after %.2fs, not right away" % elapsed)

    env.fake.behavior = respond_without_length(OVERSIZED_BODY_SIZE)
    response = get(env.fake_server.port, "/api/x")
    check(response.status == 502, "no length: status %d" % response.status)


def test_rs8_backend_connection_headers(env):
    env.fake.behavior = respond_with(
        b"HTTP/1.0 200 OK\r\nConnection: close\r\nKeep-Alive: timeout=5\r\n"
        b"Content-Length: 2\r\n\r\nok")
    response = get(env.fake_server.port, "/api/x")
    check(response.status == 200, "status %d" % response.status)
    check(response.header("Keep-Alive") is None, "Keep-Alive passed")
    check(response.header("Connection") == "keep-alive",
          "Connection %r" % response.header("Connection"))


def test_rs9_not_found_passed(env):
    env.fake.behavior = respond_with(
        b"HTTP/1.0 404 Not Found\r\nContent-Length: 4\r\n\r\nnope")
    response = get(env.fake_server.port, "/api/x")
    check(response.status == 404, "status %d" % response.status)
    check(response.body == b"nope", "body %r" % response.body)


def test_rs10_broken_status_line(env):
    env.fake.behavior = respond_with(
        b"HELLO WORLD\r\nContent-Length: 0\r\n\r\n")
    response = get(env.fake_server.port, "/api/x")
    check(response.status == 502, "status %d" % response.status)


def test_rs11_reset_without_length(env):
    env.fake.behavior = respond_then_reset(
        b"HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\n\r\npartial")
    response = get(env.fake_server.port, "/api/x")
    check(response.status == 502, "status %d" % response.status)


# ── FL. 실패 (G11, G12) ──

def test_fl1_no_backend(env):
    response = get(env.dead_server.port, "/api/hello")
    check(response.status == 502, "status %d" % response.status)


def test_fl2_connect_timeout(env):
    response, elapsed = get_timed(env.full_server, "/api/hello")
    check(response.status == 504, "status %d" % response.status)
    check_elapsed(elapsed, BACKEND_CONNECT_TIMEOUT, "504")


def test_fl3_stopped_backend(env):
    env.stopped.pause()
    try:
        response, elapsed = get_timed(env.stop_server, "/api/hello")
    finally:
        env.stopped.resume()
    check(response.status == 504, "status %d" % response.status)
    check_elapsed(elapsed, BACKEND_READ_TIMEOUT, "504")


def test_fl4_slow_backend(env):
    response = get(env.single.port, "/api/slow?ms=200")
    check(response.status == 200, "short: status %d" % response.status)
    response, elapsed = get_timed(env.single, "/api/slow?ms=2000")
    check(response.status == 504, "long: status %d" % response.status)
    check_elapsed(elapsed, BACKEND_READ_TIMEOUT, "504")


def test_fl5_backend_not_reading(env):
    env.fake.behavior = hold_without_reading(BACKEND_READ_TIMEOUT * 4)
    client = Client(env.fake_server.port)
    client.send(base.build_request(
        "/api/x", method="POST",
        headers=["Content-Length: %d" % STUCK_BODY_SIZE]))
    client.send(b"x" * STUCK_BODY_SIZE)
    response = client.read_response(10.0)
    client.close()
    check(response.status == 504, "status %d" % response.status)


# ── LB. 분배와 건너뛰기 (G13, G14) ──

def test_lb1_round_robin(env):
    ports = [fetch_hello(env.rr_server) for _ in range(10)]
    check(all(ports[i] != ports[i + 1] for i in range(9)),
          "not alternating: %r" % ports)
    for backend in (env.first, env.second):
        check(ports.count(backend.port) == 5,
              "%d got %d" % (backend.port, ports.count(backend.port)))


def test_lb2_least_conn_avoids_busy(env):
    slow = {}

    def run_slow():
        # 읽기 타임아웃(1s)보다 짧아야 504·빼기(C4)가 다음 테스트에 남지 않는다
        response = get(env.lc_server.port, "/api/slow?ms=800")
        slow["port"] = json.loads(response.body)["port"]
    thread = threading.Thread(target=run_slow)
    thread.start()
    time.sleep(0.2)
    ports = [fetch_hello(env.lc_server) for _ in range(4)]
    thread.join()
    check(len(set(ports)) == 1, "fast requests split: %r" % ports)
    check(ports[0] != slow.get("port"),
          "fast requests went to busy %r" % slow.get("port"))


def test_lb3_least_conn_tie_alternates(env):
    ports = [fetch_hello(env.lc_server) for _ in range(6)]
    check(all(ports[i] != ports[i + 1] for i in range(5)),
          "not alternating: %r" % ports)


def test_lb4_skip_dead_backend(env):
    ports = [fetch_hello(env.skip_server) for _ in range(10)]
    check(all(port == env.second.port for port in ports),
          "ports %r" % ports)
    lines = [line for line in find_proxy_lines(env.skip_server)
             if "/api/hello" in line.group(LOG_REQUEST)]
    check(len(lines) == 10, "%d proxy lines" % len(lines))
    retries = [line.group(LOG_RETRY) for line in lines]
    check(retries == ["1"] + [None] * 9, "retry tags %r" % retries)


def test_lb5_back_after_fail_timeout(env):
    time.sleep(BACKEND_FAIL_TIMEOUT + 0.3)
    env.revived.start()
    ports = [fetch_hello(env.skip_server) for _ in range(4)]
    check(env.revived.port in ports, "revived backend unused: %r" % ports)


def test_lb6_all_dead_then_one_back(env):
    response = get(env.all_dead_server.port, "/api/hello")
    check(response.status == 502, "both dead: status %d" % response.status)
    env.late.start()
    port = fetch_hello(env.all_dead_server)
    check(port == env.late.port, "answered by %d" % port)


def test_lb7_no_retry_after_send(env):
    client = Client(env.lb7_server.port)
    client.send(base.build_request("/api/x", method="POST",
                                   headers=["Content-Length: 4"],
                                   body=b"data"))
    response = client.read_response()
    client.close()
    check(response.status == 504, "status %d" % response.status)
    time.sleep(0.2)
    received = env.silent_a.count_requests() + env.silent_b.count_requests()
    check(received == 1, "backends received %d requests" % received)


def test_lb8_active_back_to_zero(env):
    time.sleep(BACKEND_READ_TIMEOUT + 0.5)
    for server in env.proxy_servers:
        for port, active in find_last_active(server).items():
            check(active == 0, "backend %d active=%d" % (port, active))


# ── CL. 끊김·자원·로그 (G18~G21) ──

def test_cl1_client_leaves_while_waiting(env):
    time.sleep(base.KEEPALIVE_TIMEOUT + base.LINGER_TOTAL + 0.3)
    before = env.single.count_fds()
    client = Client(env.single.port)
    client.send(base.build_request("/api/slow?ms=2000"))
    time.sleep(0.3)
    check(env.single.count_fds() == before + 2,
          "fds while waiting %d, expected %d"
          % (env.single.count_fds(), before + 2))
    client.close()
    time.sleep(0.3)
    check(env.single.count_fds() == before,
          "fds %d after client left, expected %d"
          % (env.single.count_fds(), before))
    active = find_last_active(env.single).get(env.first.port)
    check(active == 0, "backend active=%r" % active)


def test_cl2_fd_limit_halves(env):
    server = Server(env.binary, env.root.root, env.root.base, "limit",
                    make_options(["/api round_robin "
                                  + backend_list(env.first.port)],
                                 header_timeout="10000ms",
                                 keepalive_timeout="10000ms",
                                 backend_read_timeout="5000ms"),
                    fd_limit=FD_LIMIT_FOR_CL2)
    clients = []
    try:
        for _ in range(CL2_CLIENT_COUNT):
            client = Client(server.port)
            client.send(base.build_request("/api/slow?ms=%d" % CL2_SLOW_MS))
            clients.append(client)
        time.sleep(CL2_SLOW_MS / 1000 + 0.8)
        served = [client for client in clients if client.has_data_now()]
        check(len(served) == EXPECTED_MAX_CONNECTIONS,
              "%d served in first wave, expected %d"
              % (len(served), EXPECTED_MAX_CONNECTIONS))
        # 받은 손님이 keep-alive로 자리를 쥐고 있으면 대기 손님이 못 들어온다
        # (3단계 I-2와 같은 방식)
        for client in served:
            response = client.read_response(10.0)
            check(response.status == 200, "status %d" % response.status)
            client.close()
            clients.remove(client)
        for client in clients:
            response = client.read_response(10.0)
            check(response.status == 200, "status %d" % response.status)
        stderr = server.read_stderr()
        check("max connections: %d" % EXPECTED_MAX_CONNECTIONS in stderr,
              "start line missing")
        check("Too many open files" not in stderr, "EMFILE in stderr")
    finally:
        for client in clients:
            client.close()
        server.stop()


def test_cl3_proxy_log(env):
    single_lines = find_proxy_lines(env.single)
    ok_lines = [line for line in single_lines
                if line.group(LOG_STATUS) == "200"
                and int(line.group(LOG_BACKEND_PORT)) == env.first.port]
    check(ok_lines, "no 200 line with -> 127.0.0.1:%d" % env.first.port)
    retry_lines = [line for line in find_proxy_lines(env.skip_server)
                   if line.group(LOG_RETRY) == "1"]
    check(retry_lines, "no retry=1 line")
    timeout_lines = [line for line in single_lines
                     if line.group(LOG_STATUS) == "504"
                     and line.group(LOG_CAUSE) == "timeout"]
    check(timeout_lines, "no 504 line with timeout")


def test_cl4_fds_and_cards(env):
    time.sleep(base.LINGER_TOTAL + base.KEEPALIVE_TIMEOUT + 0.5)
    fds = env.single.count_fds()
    check(fds == env.single_baseline_fds,
          "fds %d, at start %d" % (fds, env.single_baseline_fds))
    counts = [base.COUNT_LOG_PATTERN.match(line)
              for line in env.single.read_stderr().splitlines()]
    counts = [match for match in counts if match is not None]
    check(counts, "no count line in stderr (count_log on)")
    created, destroyed = counts[-1].group(1), counts[-1].group(2)
    check(created == destroyed, "created %s, destroyed %s"
          % (created, destroyed))


def test_cl5_no_bad_access(env):
    for server in env.proxy_servers:
        check(server.is_alive(), "server process exited")
        check("AddressSanitizer" not in server.read_stderr(),
              "AddressSanitizer report in stderr")


def test_cl6_step3_tests(env):
    finished = subprocess.run(
        [sys.executable, STEP3_TEST_SCRIPT, env.binary],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=600)
    output = finished.stdout.decode("utf-8", "replace")
    summary = [line for line in output.splitlines()
               if line.startswith("통과") or line.startswith("실패한 ID")]
    print("  3단계: " + " / ".join(summary), flush=True)
    check(finished.returncode == 0, "step 3 tests failed")


# ── 실행 순서 ──

class Environment:
    pass


def start_environment(env):
    env.first = Backend()
    env.second = Backend()
    env.stopped = Backend()
    env.revived = Backend()
    env.late = Backend()
    env.dead_port = base.pick_free_port()
    env.other_dead_port = base.pick_free_port()
    for backend in (env.first, env.second, env.stopped):
        backend.start()
    env.fake = FakeBackend(receive_buffer=SMALL_RECEIVE_BUFFER)
    env.silent_a = FakeBackend(read_then_hold(BACKEND_READ_TIMEOUT * 3))
    env.silent_b = FakeBackend(read_then_hold(BACKEND_READ_TIMEOUT * 3))
    env.full = FullListener()

    def start_server(name, routes, **overrides):
        server = Server(env.binary, env.root.root, env.root.base, name,
                        make_options(routes, **overrides))
        env.proxy_servers.append(server)
        return server

    env.proxy_servers = []
    env.single = start_server(
        "single", ["/api round_robin " + backend_list(env.first.port)])
    env.fake_server = start_server(
        "fake", ["/api round_robin " + backend_list(env.fake.port)],
        max_body=STUCK_MAX_BODY)
    env.dead_server = start_server(
        "dead", ["/api round_robin " + backend_list(env.dead_port)])
    env.full_server = start_server(
        "full", ["/api round_robin " + backend_list(env.full.port)])
    env.stop_server = start_server(
        "stop", ["/api round_robin " + backend_list(env.stopped.port)])
    env.rr_server = start_server(
        "rr", ["/api round_robin "
               + backend_list(env.first.port, env.second.port)])
    env.lc_server = start_server(
        "lc", ["/api least_conn "
               + backend_list(env.first.port, env.second.port)])
    env.skip_server = start_server(
        "skip", ["/api round_robin "
                 + backend_list(env.revived.port, env.second.port)])
    env.all_dead_server = start_server(
        "alldead", ["/api round_robin "
                    + backend_list(env.other_dead_port, env.late.port)],
        backend_fail_timeout=LONG_FAIL_TIMEOUT)
    env.lb7_server = start_server(
        "lb7", ["/api round_robin "
                + backend_list(env.silent_a.port, env.silent_b.port)])
    time.sleep(0.3)
    env.single_baseline_fds = env.single.count_fds()


def stop_environment(env):
    for server in getattr(env, "proxy_servers", []):
        server.stop()
    for name in ("first", "second", "stopped", "revived", "late"):
        backend = getattr(env, name, None)
        if backend is not None:
            backend.stop()
    for name in ("fake", "silent_a", "silent_b", "full"):
        fake = getattr(env, name, None)
        if fake is not None:
            fake.stop()


ENVIRONMENT_TESTS = [
    ("HD-1", test_hd1_hop_by_hop_removed),
    ("HD-2", test_hd2_host_kept),
    ("HD-3", test_hd3_forwarded_for),
    ("HD-4", test_hd4_raw_path_http10),
    ("HD-5", test_hd5_expect_removed),
    ("EX-1", test_ex1_continue_then_echo),
    ("EX-2", test_ex2_too_large_no_continue),
    ("EX-3", test_ex3_no_one_second_wait),
    ("RS-1", test_rs1_length_response_keepalive),
    ("RS-2", test_rs2_close_delimited_gets_length),
    ("RS-3", test_rs3_short_body_502),
    ("RS-4", test_rs4_chunked_502),
    ("RS-5", test_rs5_skip_continue),
    ("RS-6", test_rs6_no_content_keepalive),
    ("RS-7", test_rs7_oversized_502),
    ("RS-8", test_rs8_backend_connection_headers),
    ("RS-9", test_rs9_not_found_passed),
    ("RS-10", test_rs10_broken_status_line),
    ("RS-11", test_rs11_reset_without_length),
    ("FL-1", test_fl1_no_backend),
    ("FL-2", test_fl2_connect_timeout),
    ("FL-3", test_fl3_stopped_backend),
    ("FL-4", test_fl4_slow_backend),
    ("FL-5", test_fl5_backend_not_reading),
    ("LB-1", test_lb1_round_robin),
    ("LB-2", test_lb2_least_conn_avoids_busy),
    ("LB-3", test_lb3_least_conn_tie_alternates),
    ("LB-4", test_lb4_skip_dead_backend),
    ("LB-5", test_lb5_back_after_fail_timeout),
    ("LB-6", test_lb6_all_dead_then_one_back),
    ("LB-7", test_lb7_no_retry_after_send),
    ("CL-1", test_cl1_client_leaves_while_waiting),
    ("CL-2", test_cl2_fd_limit_halves),
    ("CL-3", test_cl3_proxy_log),
    ("LB-8", test_lb8_active_back_to_zero),
    ("CL-4", test_cl4_fds_and_cards),
    ("CL-5", test_cl5_no_bad_access),
]


def main():
    """뒤에 ID를 주면 그 테스트만 돌린다 (고치는 중 다시 돌리기용)"""
    if len(sys.argv) < 2:
        print("usage: %s <server binary> [test ID...]" % sys.argv[0],
              file=sys.stderr)
        return 2
    selected = set(sys.argv[2:])
    env = Environment()
    env.binary = os.path.abspath(sys.argv[1])
    env.root = ProxyRoot()
    try:
        start_environment(env)
        for test_id, function in ENVIRONMENT_TESTS:
            if not selected or test_id in selected:
                run_test(test_id, function, env)
    finally:
        stop_environment(env)
        env.root.remove()
    if not selected or "CL-6" in selected:
        run_test("CL-6", test_cl6_step3_tests, env)
    print("통과 %d, 실패 %d" % (results["pass"], results["fail"]))
    if results["failed_ids"]:
        print("실패한 ID: " + ", ".join(results["failed_ids"]))
    return 1 if results["fail"] else 0


if __name__ == "__main__":
    sys.exit(main())
