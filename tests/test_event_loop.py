"""3단계 통합 테스트 (docs/step3-loop.md 5번 A~M, L4·L42)

사용: python3 tests/test_event_loop.py <서버 실행 파일>

서버를 직접 띄우고 끈다. 정적 루트와 파일은 실행 중 /tmp 아래에 만들고
끝나면 지운다 (S18). 타임아웃은 L35의 짧은 값으로 돌린다.
리눅스 전용 (/proc, D5).
"""

import hashlib
import os
import re
import resource
import select
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time

# ── 서버 설정 (L35) ──

HEADER_TIMEOUT = 1.0
SEND_TIMEOUT = 1.0
KEEPALIVE_TIMEOUT = 1.0
LINGER_QUIET = 0.3
LINGER_TOTAL = 1.5

SHORT_TIMEOUT_OPTIONS = [
    "--header-timeout-ms=1000",
    "--send-timeout-ms=1000",
    "--keepalive-timeout-ms=1000",
    "--linger-quiet-ms=300",
    "--linger-total-ms=1500",
]

# 시간 비교 여유: ASan 빌드와 Docker에서 늦어지는 만큼 위쪽으로만 넉넉히 둔다
LATE_MARGIN = 1.0

# (L42) 손님 받기 버퍼를 작게 잡아서 "안 읽어 감"이 서버 쓰기를 막게 한다
SMALL_RECEIVE_BUFFER = 4096

# (I-1, L8) (64 − (16 + 1 × 2)) ÷ 2
FD_LIMIT_FOR_I1 = 64
EXPECTED_MAX_CONNECTIONS = 23

# (H-3, I-1) "CPU 거의 0%": 측정 시간 대비 CPU 시간 비율 상한
IDLE_CPU_RATIO = 0.05

ACCESS_LOG_PATTERN = re.compile(
    r'^\[\d{2}/[A-Z][a-z]{2}/\d{4}:\d{2}:\d{2}:\d{2} \+0000\] '
    r'(\d{1,3}(?:\.\d{1,3}){3}):(\d+) "([^"]*)" (\d{3}|-) (\d+) (\d+)'
    r'(?: (close|reset|timeout))?$')
COUNT_LOG_PATTERN = re.compile(r'^cards created=(\d+) destroyed=(\d+)$')


# ── 결과 집계 ──

class CheckFailed(Exception):
    pass


def check(condition, message):
    if not condition:
        raise CheckFailed(message)


results = {"pass": 0, "fail": 0, "failed_ids": []}


def run_test(test_id, function, *arguments):
    try:
        function(*arguments)
    except CheckFailed as error:
        record_failure(test_id, str(error))
        return
    except Exception as error:  # 소켓 오류도 실패로 세고 계속 간다
        record_failure(test_id, "%s: %s" % (type(error).__name__, error))
        return
    results["pass"] += 1
    print("[%s] PASS" % test_id, flush=True)


def record_failure(test_id, message):
    results["fail"] += 1
    results["failed_ids"].append(test_id)
    print("[%s] FAIL: %s" % (test_id, message), flush=True)


# ── 정적 루트 (S18) ──

class StaticRoot:
    def __init__(self):
        self.base = tempfile.mkdtemp(prefix="step3-")
        self.root = os.path.join(self.base, "www")
        os.mkdir(self.root)
        self.files = {}
        self.add("index.html", b"<html><body>index</body></html>\n")
        self.add("a.html", b"<html><body>a</body></html>\n")
        self.add("b.html", b"<html><body>b-page</body></html>\n")
        self.add("bin1m.bin", os.urandom(1024 * 1024))
        self.add("f8192.bin", os.urandom(8192))
        self.add("f8193.bin", os.urandom(8193))
        self.add("empty.txt", b"")
        self.add("noperm.html", b"secret\n")
        os.chmod(self.path("noperm.html"), 0)
        os.mkdir(self.path("docs"))
        self.add("docs/index.html", b"<html><body>docs</body></html>\n")
        self.add("big.bin", os.urandom(8 * 1024 * 1024))
        self.add("shrink.bin", os.urandom(8 * 1024 * 1024))

    def add(self, name, content):
        with open(self.path(name), "wb") as file:
            file.write(content)
        self.files[name] = content

    def path(self, name):
        return os.path.join(self.root, name)

    def remove(self):
        os.chmod(self.path("noperm.html"), 0o644)
        shutil.rmtree(self.base)


# ── 서버 띄우기·끄기 ──

class Server:
    def __init__(self, binary, root, base, name, options, fd_limit=None):
        self.port = pick_free_port()
        self.stdout_path = os.path.join(base, name + ".stdout")
        self.stderr_path = os.path.join(base, name + ".stderr")
        self.stdout_file = open(self.stdout_path, "wb")
        self.stderr_file = open(self.stderr_path, "wb")
        arguments = [binary, str(self.port), root] + options
        self.process = subprocess.Popen(
            arguments, stdout=self.stdout_file, stderr=self.stderr_file,
            preexec_fn=make_fd_limiter(fd_limit))
        wait_until_listening(self.port)

    def is_alive(self):
        return self.process.poll() is None

    def read_stdout(self):
        with open(self.stdout_path, "rb") as file:
            return file.read().decode("utf-8", "replace")

    def read_stderr(self):
        with open(self.stderr_path, "rb") as file:
            return file.read().decode("utf-8", "replace")

    def count_fds(self):
        return len(os.listdir("/proc/%d/fd" % self.process.pid))

    def read_cpu_seconds(self):
        with open("/proc/%d/stat" % self.process.pid) as file:
            fields = file.read().rsplit(")", 1)[1].split()
        ticks = int(fields[11]) + int(fields[12])
        return ticks / os.sysconf("SC_CLK_TCK")

    def stop(self):
        if self.is_alive():
            self.process.terminate()
            try:
                self.process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait()
        self.stdout_file.close()
        self.stderr_file.close()


def pick_free_port():
    probe = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    probe.bind(("127.0.0.1", 0))
    port = probe.getsockname()[1]
    probe.close()
    return port


def make_fd_limiter(fd_limit):
    if fd_limit is None:
        return None

    def limit_fds():
        hard = resource.getrlimit(resource.RLIMIT_NOFILE)[1]
        resource.setrlimit(resource.RLIMIT_NOFILE, (fd_limit, hard))
    return limit_fds


def wait_until_listening(port):
    deadline = time.monotonic() + 5.0
    while time.monotonic() < deadline:
        try:
            probe = socket.create_connection(("127.0.0.1", port), timeout=0.2)
            probe.close()
            return
        except OSError:
            time.sleep(0.05)
    raise RuntimeError("server did not start listening on %d" % port)


# ── 손님 ──

class Response:
    def __init__(self, status, headers, body):
        self.status = status
        self.headers = headers
        self.body = body

    def header(self, name):
        return self.headers.get(name.lower())


class Client:
    def __init__(self, port, receive_buffer=None):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        if receive_buffer is not None:
            self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF,
                                 receive_buffer)
        self.sock.settimeout(5.0)
        self.sock.connect(("127.0.0.1", port))
        self.buffer = b""

    def local_port(self):
        return self.sock.getsockname()[1]

    def send(self, data):
        self.sock.sendall(data)

    def read_response(self, timeout=5.0):
        self.sock.settimeout(timeout)
        while b"\r\n\r\n" not in self.buffer:
            self.receive_more()
        head, self.buffer = self.buffer.split(b"\r\n\r\n", 1)
        status, headers = parse_head(head)
        length = int(headers.get("content-length", "0"))
        while len(self.buffer) < length:
            self.receive_more()
        body, self.buffer = self.buffer[:length], self.buffer[length:]
        return Response(status, headers, body)

    def read_head_only(self, timeout=5.0):
        self.sock.settimeout(timeout)
        while b"\r\n\r\n" not in self.buffer:
            self.receive_more()
        head, self.buffer = self.buffer.split(b"\r\n\r\n", 1)
        status, headers = parse_head(head)
        return Response(status, headers, b"")

    def receive_more(self):
        chunk = self.sock.recv(65536)
        if chunk == b"":
            raise CheckFailed("connection closed before full response")
        self.buffer += chunk

    def read_until_close(self, timeout):
        """반환: (받은 바이트, "eof" / "reset" / "timeout")"""
        data, self.buffer = self.buffer, b""
        deadline = time.monotonic() + timeout
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                return data, "timeout"
            self.sock.settimeout(remaining)
            try:
                chunk = self.sock.recv(65536)
            except socket.timeout:
                return data, "timeout"
            except ConnectionResetError:
                return data, "reset"
            if chunk == b"":
                return data, "eof"
            data += chunk

    def wait_silence(self, seconds):
        """반환: "silent" / "data" / "eof" / "reset" """
        if self.buffer:
            return "data"
        self.sock.settimeout(seconds)
        try:
            chunk = self.sock.recv(1, socket.MSG_PEEK)
        except socket.timeout:
            return "silent"
        except ConnectionResetError:
            return "reset"
        return "eof" if chunk == b"" else "data"

    def has_data_now(self):
        if self.buffer:
            return True
        readable, _, _ = select.select([self.sock], [], [], 0)
        return bool(readable)

    def close_with_reset(self):
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER,
                             struct.pack("ii", 1, 0))
        self.sock.close()

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass


def parse_head(head):
    lines = head.decode("latin-1").split("\r\n")
    status = int(lines[0].split(" ")[1])
    headers = {}
    for line in lines[1:]:
        name, value = line.split(":", 1)
        headers[name.strip().lower()] = value.strip()
    return status, headers


def build_request(path, method="GET", headers=(), body=b""):
    lines = ["%s %s HTTP/1.1" % (method, path), "Host: localhost"]
    lines.extend(headers)
    return ("\r\n".join(lines) + "\r\n\r\n").encode("latin-1") + body


def get(port, path, headers=()):
    client = Client(port)
    client.send(build_request(path, headers=headers))
    response = client.read_response()
    client.close()
    return response


def check_server_alive(server):
    check(server.is_alive(), "server process exited")
    response = get(server.port, "/a.html")
    check(response.status == 200, "server does not answer after the test")


def check_elapsed(elapsed, expected, what):
    lower = expected * 0.8
    upper = expected + LATE_MARGIN
    check(lower <= elapsed <= upper,
          "%s after %.2fs, expected about %.2fs" % (what, elapsed, expected))


def check_closed(how):
    check(how in ("eof", "reset"), "connection not closed (%s)" % how)


def find_server_socket_inode(server_port, client_port):
    """서버 쪽 소켓의 inode. 0 = 서버가 close함(커널만 남음), None = 줄 없음"""
    with open("/proc/net/tcp") as file:
        lines = file.read().splitlines()[1:]
    for line in lines:
        fields = line.split()
        local_port = int(fields[1].split(":")[1], 16)
        remote_port = int(fields[2].split(":")[1], 16)
        if local_port == server_port and remote_port == client_port:
            return int(fields[9])
    return None


def wait_server_close(server, client, limit):
    """(L12) lingering은 shutdown(SHUT_WR)에서 FIN이 먼저 나가서 손님 쪽
    EOF로는 진짜 close를 알 수 없다. 서버가 소켓을 놓은 시점까지 걸린 초,
    limit 안에 안 닫히면 None"""
    client_port = client.local_port()
    started = time.monotonic()
    while time.monotonic() - started < limit:
        inode = find_server_socket_inode(server.port, client_port)
        if inode is None or inode == 0:
            return time.monotonic() - started
        time.sleep(0.01)
    return None


def check_server_closed(server, client):
    closed_after = wait_server_close(server, client,
                                     LINGER_TOTAL + LATE_MARGIN)
    check(closed_after is not None, "server did not close the socket")


def describe_lines(lines):
    """실패 메시지에 찾은 줄 내용을 같이 남겨서 원인을 바로 볼 수 있게"""
    return "".join("\n    " + match.group(0) for match in lines)


def find_log_lines(server, client_port):
    lines = []
    for line in server.read_stdout().splitlines():
        match = ACCESS_LOG_PATTERN.match(line)
        if match is not None and int(match.group(2)) == client_port:
            lines.append(match)
    return lines


# ── A. 기본 전송 (R1a, R1f, L9) ──

def test_a1_get_index(server, root):
    response = get(server.port, "/index.html")
    check(response.status == 200, "status %d" % response.status)
    check(response.body == root.files["index.html"], "body differs")


def test_a2_one_megabyte_hash(server, root):
    response = get(server.port, "/bin1m.bin")
    check(response.status == 200, "status %d" % response.status)
    expected = hashlib.sha256(root.files["bin1m.bin"]).hexdigest()
    actual = hashlib.sha256(response.body).hexdigest()
    check(actual == expected, "hash differs")


def test_a3_chunk_boundary(server, root):
    for name in ("f8192.bin", "f8193.bin"):
        response = get(server.port, "/" + name)
        check(response.status == 200, "%s status %d" % (name, response.status))
        check(response.body == root.files[name], "%s body differs" % name)


def test_a4_empty_file(server, root):
    client = Client(server.port)
    client.send(build_request("/empty.txt"))
    response = client.read_response()
    check(response.status == 200, "status %d" % response.status)
    check(response.header("Content-Length") == "0", "Content-Length not 0")
    check(client.wait_silence(0.2) == "silent", "bytes after empty response")
    client.close()


def test_a5_static_errors_over_socket(server, root):
    response = get(server.port, "/missing.html")
    check(response.status == 404, "missing: status %d" % response.status)
    response = get(server.port, "/noperm.html")
    check(response.status == 403, "noperm: status %d" % response.status)
    response = get(server.port, "/docs")
    check(response.status == 301, "docs: status %d" % response.status)
    check(response.header("Location") == "/docs/",
          "Location %r" % response.header("Location"))


# ── B. 조각과 붙음 (R3c, L13) ──

def test_b1_one_byte_at_a_time(server, root):
    client = Client(server.port)
    for byte in build_request("/a.html"):
        client.send(bytes([byte]))
        time.sleep(0.01)
    response = client.read_response()
    check(response.status == 200, "status %d" % response.status)
    check(response.body == root.files["a.html"], "body differs")
    client.close()


def test_b2_two_requests_in_one_send(server, root):
    client = Client(server.port)
    client.send(build_request("/a.html") + build_request("/b.html"))
    first = client.read_response()
    second = client.read_response()
    check(first.body == root.files["a.html"], "first response is not a.html")
    check(second.body == root.files["b.html"], "second response is not b.html")
    client.close()


def test_b3_big_then_small(server, root):
    client = Client(server.port)
    client.send(build_request("/bin1m.bin") + build_request("/a.html"))
    first = client.read_response()
    second = client.read_response()
    check(first.body == root.files["bin1m.bin"], "first response is not big")
    check(second.body == root.files["a.html"], "second response is not small")
    client.close()


def test_b4_second_request_split(server, root):
    client = Client(server.port)
    second = build_request("/b.html")
    half = len(second) // 2
    client.send(build_request("/a.html") + second[:half])
    time.sleep(0.2)
    client.send(second[half:])
    first_response = client.read_response()
    second_response = client.read_response()
    check(first_response.body == root.files["a.html"], "first body differs")
    check(second_response.body == root.files["b.html"], "second body differs")
    client.close()


# ── C. keep-alive와 연결 유지 (R3d, L10, B31) ──

def test_c1_three_gets_same_connection(server, root):
    client = Client(server.port)
    for _ in range(3):
        client.send(build_request("/a.html"))
        response = client.read_response()
        check(response.status == 200, "status %d" % response.status)
        check(response.header("Connection") == "keep-alive",
              "Connection %r" % response.header("Connection"))
    check(client.wait_silence(0.2) == "silent", "connection not kept")
    client.close()


def test_c2_connection_close(server, root):
    client = Client(server.port)
    client.send(build_request("/a.html", headers=["Connection: close"]))
    response = client.read_response()
    check(response.header("Connection") == "close",
          "Connection %r" % response.header("Connection"))
    check_server_closed(server, client)
    client.close()


def test_c3_keep_after_404_403_301(server, root):
    client = Client(server.port)
    for path, status in (("/missing.html", 404), ("/noperm.html", 403),
                         ("/docs", 301)):
        client.send(build_request(path))
        response = client.read_response()
        check(response.status == status, "%s: status %d"
              % (path, response.status))
        client.send(build_request("/a.html"))
        response = client.read_response()
        check(response.status == 200, "GET after %d failed" % status)
    client.close()


def test_c4_static_400_closes(server, root):
    # 500은 바깥에서 만들 방법이 없어 이 테스트에 없다 (보고 참고)
    client = Client(server.port)
    client.send(build_request("/%zz"))
    response = client.read_response()
    check(response.status == 400, "status %d" % response.status)
    check(response.header("Connection") == "close",
          "Connection %r" % response.header("Connection"))
    check_server_closed(server, client)
    client.close()


def test_c5_parser_400_closes(server, root):
    client = Client(server.port)
    client.send(b"HELLO\r\n\r\n")
    response = client.read_response()
    check(response.status == 400, "status %d" % response.status)
    check(response.header("Connection") == "close",
          "Connection %r" % response.header("Connection"))
    check_server_closed(server, client)
    client.close()


# ── D. 본문 버리기 (L11, L21, P10) ──

def test_d1_post_405_then_get(server, root):
    client = Client(server.port)
    client.send(build_request("/a.html", method="POST",
                              headers=["Content-Length: 10"],
                              body=b"0123456789"))
    response = client.read_response()
    check(response.status == 405, "status %d" % response.status)
    check(response.header("Allow") == "GET",
          "Allow %r" % response.header("Allow"))
    client.send(build_request("/a.html"))
    response = client.read_response()
    check(response.status == 200, "GET after 405: status %d"
          % response.status)
    client.close()


def test_d2_get_with_body_then_get(server, root):
    client = Client(server.port)
    client.send(build_request("/a.html", headers=["Content-Length: 5"],
                              body=b"hello"))
    first = client.read_response()
    check(first.status == 200, "first status %d" % first.status)
    check(first.body == root.files["a.html"], "first body differs")
    client.send(build_request("/b.html"))
    second = client.read_response()
    check(second.status == 200, "second status %d" % second.status)
    check(second.body == root.files["b.html"], "second body differs")
    client.close()


def test_d3_post_expect_closes(server, root):
    client = Client(server.port)
    client.send(build_request("/a.html", method="POST",
                              headers=["Content-Length: 10",
                                       "Expect: 100-continue"]))
    response = client.read_response()
    check(response.status == 405, "status %d" % response.status)
    check(response.header("Connection") == "close",
          "Connection %r" % response.header("Connection"))
    client.close()


def test_d4_body_stops_after_405(server, root):
    client = Client(server.port)
    client.send(build_request("/a.html", method="POST",
                              headers=["Content-Length: 1000"],
                              body=b"0123456789"))
    response = client.read_response()
    check(response.status == 405, "status %d" % response.status)
    started = time.monotonic()
    _, how = client.read_until_close(LINGER_TOTAL + LATE_MARGIN)
    check_closed(how)
    check_elapsed(time.monotonic() - started, LINGER_QUIET, "closed")
    client.close()


def test_d5_body_trickles_after_405(server, root):
    client = Client(server.port)
    client.send(build_request("/a.html", method="POST",
                              headers=["Content-Length: 100000"]))
    response = client.read_response()
    check(response.status == 405, "status %d" % response.status)
    started = time.monotonic()
    closed_after = trickle_until_closed(client, 0.1,
                                        LINGER_TOTAL + LATE_MARGIN + 1.0)
    check(closed_after is not None, "never closed while trickling")
    check_elapsed(closed_after - started, LINGER_TOTAL, "closed")
    client.close()


def trickle_until_closed(client, interval, limit):
    """interval마다 1바이트씩 보내다가 닫힘을 본 시각을 돌려준다. 없으면 None"""
    deadline = time.monotonic() + limit
    while time.monotonic() < deadline:
        try:
            client.sock.send(b"x")
        except (BrokenPipeError, ConnectionResetError):
            return time.monotonic()
        if client.wait_silence(interval) in ("eof", "reset"):
            return time.monotonic()
    return None


# ── E. 타임아웃과 느린 손님 (L5, L6) ──

def test_e1_half_header_408(server, root):
    client = Client(server.port)
    client.send(b"GET /a.html HTTP/1.1\r\nHost: lo")
    started = time.monotonic()
    response = client.read_response(HEADER_TIMEOUT + LATE_MARGIN + 1.0)
    check_elapsed(time.monotonic() - started, HEADER_TIMEOUT, "408")
    check(response.status == 408, "status %d" % response.status)
    check(response.header("Connection") == "close",
          "Connection %r" % response.header("Connection"))
    check_server_closed(server, client)
    client.close()


def test_e2_slowloris_408(server, root):
    client = Client(server.port)
    client.send(b"GET /a.html HTTP/1.1\r\nHost: localhost\r\nX-Slow: ")
    started = time.monotonic()
    deadline = started + HEADER_TIMEOUT + LATE_MARGIN + 1.0
    while time.monotonic() < deadline and not client.has_data_now():
        try:
            client.sock.send(b"x")
        except (BrokenPipeError, ConnectionResetError):
            break
        time.sleep(0.1)
    response = client.read_response(1.0)
    check_elapsed(time.monotonic() - started, HEADER_TIMEOUT, "408")
    check(response.status == 408, "status %d" % response.status)
    client.close()


def test_e4_idle_close_silently(server, root):
    client = Client(server.port)
    client.send(build_request("/a.html"))
    client.read_response()
    started = time.monotonic()
    data, how = client.read_until_close(KEEPALIVE_TIMEOUT + LATE_MARGIN + 1.0)
    check_closed(how)
    check(data == b"", "received %d bytes before close" % len(data))
    check_elapsed(time.monotonic() - started, KEEPALIVE_TIMEOUT, "closed")
    client.close()


def test_e5_send_blocked_closes(server, root):
    client = Client(server.port, SMALL_RECEIVE_BUFFER)
    client.send(build_request("/big.bin"))
    time.sleep(SEND_TIMEOUT + LATE_MARGIN + 1.0)
    data, how = client.read_until_close(10.0)
    check_closed(how)
    check(len(data) < len(root.files["big.bin"]),
          "whole file arrived; send was never blocked")
    client.close()


def test_e6_other_client_while_slow(server, root):
    slow = Client(server.port)
    slow.send(b"GET /a.html HTTP/1.1\r\nHost: lo")
    started = time.monotonic()
    response = get(server.port, "/a.html")
    check(response.status == 200, "status %d" % response.status)
    check(time.monotonic() - started < 0.5, "other client waited")
    slow.close()


# ── F. lingering close (L12) ──

def open_413_while_sending(server):
    """Content-Length 2,000,000 POST를 보내면서 413 헤더를 읽는다"""
    client = Client(server.port)
    client.send(build_request("/a.html", method="POST",
                              headers=["Content-Length: 2000000"]))
    client.sock.setblocking(False)
    chunk = b"x" * 65536
    deadline = time.monotonic() + 3.0
    while b"\r\n\r\n" not in client.buffer and time.monotonic() < deadline:
        _, writable, _ = select.select([], [client.sock], [], 0.01)
        if writable:
            send_ignoring_full(client.sock, chunk)
        readable, _, _ = select.select([client.sock], [], [], 0.01)
        if readable:
            received = client.sock.recv(65536)
            check(received != b"", "closed before 413 arrived")
            client.buffer += received
    client.sock.setblocking(True)
    return client, client.read_head_only(1.0)


def send_ignoring_full(sock, data):
    try:
        sock.send(data)
    except BlockingIOError:
        pass


def test_f1_413_not_reset(server, root):
    client, response = open_413_while_sending(server)
    check(response.status == 413, "status %d" % response.status)
    client.close()


def test_f2_quiet_after_413(server, root):
    client, response = open_413_while_sending(server)
    check(response.status == 413, "status %d" % response.status)
    closed_after = wait_server_close(server, client,
                                     LINGER_TOTAL + LATE_MARGIN)
    check(closed_after is not None, "server did not close the socket")
    check_elapsed(closed_after, LINGER_QUIET, "closed")
    client.close()


def test_f3_keep_sending_after_413(server, root):
    client, response = open_413_while_sending(server)
    check(response.status == 413, "status %d" % response.status)
    started = time.monotonic()
    closed_after = trickle_until_closed(client, 0.05,
                                        LINGER_TOTAL + LATE_MARGIN + 1.0)
    check(closed_after is not None, "never closed while sending")
    check_elapsed(closed_after - started, LINGER_TOTAL, "closed")
    client.close()


# ── G. 끊김 (R6a, L14, L17, Q9) ──

def test_g1_close_right_away(server, root):
    Client(server.port).close()
    time.sleep(0.1)
    check_server_alive(server)


def test_g2_half_header_then_close(server, root):
    client = Client(server.port)
    client.send(b"GET /a.html HTTP/1.1\r\nHost: lo")
    client.close()
    time.sleep(0.1)
    check_server_alive(server)


def reset_during_big_download(server):
    client = Client(server.port, SMALL_RECEIVE_BUFFER)
    client.send(build_request("/big.bin"))
    received = 0
    while received < 10 * 1024:
        chunk = client.sock.recv(4096)
        check(chunk != b"", "closed before 10KB")
        received += len(chunk)
    port = client.local_port()
    client.close_with_reset()
    return port


def test_g3_reset_during_download(server, root):
    port = reset_during_big_download(server)
    time.sleep(0.5)
    check_server_alive(server)
    lines = find_log_lines(server, port)
    check(len(lines) == 1, "%d log lines for the client%s"
          % (len(lines), describe_lines(lines)))
    check(lines[0].group(7) == "reset", "tag %r" % lines[0].group(7))


def test_g4_half_close_after_request(server, root):
    # (L27) Connection 헤더는 FIN 도착 순서에 따라 달라서 검사하지 않는다
    client = Client(server.port)
    client.send(build_request("/a.html"))
    client.sock.shutdown(socket.SHUT_WR)
    response = client.read_response()
    check(response.status == 200, "status %d" % response.status)
    check(response.body == root.files["a.html"], "body differs")
    check_server_closed(server, client)
    client.close()


def test_g5_reset_hundred_times(server, root):
    for _ in range(100):
        reset_during_big_download(server)
    time.sleep(0.5)
    check_server_alive(server)


# ── H. 동시성 (Q3, L3) ──

def run_in_threads(count, work):
    outcomes = [None] * count
    barrier = threading.Barrier(count)

    def run(index):
        try:
            barrier.wait()
            outcomes[index] = work()
        except Exception as error:
            outcomes[index] = error

    threads = [threading.Thread(target=run, args=(i,)) for i in range(count)]
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join()
    return outcomes


def test_h1_fifty_concurrent(server, root):
    expected = root.files["bin1m.bin"]

    def work():
        response = get(server.port, "/bin1m.bin")
        return response.status == 200 and response.body == expected

    outcomes = run_in_threads(50, work)
    good = sum(1 for outcome in outcomes if outcome is True)
    check(good == 50, "%d of 50 correct" % good)


def test_h2_slow_and_normal(server, root):
    slow_clients = []
    for _ in range(5):
        client = Client(server.port)
        client.send(b"GET /a.html HTTP/1.1\r\nHost: lo")
        slow_clients.append(client)
    for _ in range(5):
        client = Client(server.port, SMALL_RECEIVE_BUFFER)
        client.send(build_request("/big.bin"))
        slow_clients.append(client)

    def work():
        started = time.monotonic()
        response = get(server.port, "/a.html")
        return response.status == 200, time.monotonic() - started

    outcomes = run_in_threads(50, work)
    for client in slow_clients:
        client.close()
    bad = [outcome for outcome in outcomes
           if not isinstance(outcome, tuple) or not outcome[0]
           or outcome[1] >= 1.0]
    check(not bad, "%d of 50 normal clients failed or took 1s+" % len(bad))


def test_h3_idle_keepalive_cpu(server, root):
    clients = []
    for _ in range(20):
        client = Client(server.port)
        client.send(build_request("/a.html"))
        client.read_response()
        clients.append(client)
    ratio = measure_cpu_ratio(server, 0.7)
    for client in clients:
        client.close()
    check(ratio < IDLE_CPU_RATIO, "CPU %.0f%%" % (ratio * 100))


def measure_cpu_ratio(server, seconds):
    cpu_before = server.read_cpu_seconds()
    time.sleep(seconds)
    return (server.read_cpu_seconds() - cpu_before) / seconds


# ── I. 연결 한도 (L8, L22, L23) ──

def test_i_connection_limit(binary, root):
    # 대기 중인 손님이 타임아웃으로 풀려나지 않도록 헤더·idle만 길게 둔다
    server = Server(binary, root.root, root.base, "limit",
                    ["--header-timeout-ms=10000",
                     "--keepalive-timeout-ms=10000"],
                    fd_limit=FD_LIMIT_FOR_I1)
    clients = []
    try:
        run_test("I-1", check_overflow_waits, server, clients)
        run_test("I-2", check_waiting_served, server, clients)
    finally:
        for client in clients:
            client.close()
        server.stop()


def check_overflow_waits(server, clients):
    for _ in range(100):
        client = Client(server.port)
        client.send(build_request("/a.html"))
        clients.append(client)
    time.sleep(0.5)
    served = collect_served(clients)
    check(len(served) == EXPECTED_MAX_CONNECTIONS,
          "%d served, expected %d" % (len(served), EXPECTED_MAX_CONNECTIONS))
    # 읽을 게 없다 = 응답도, 거절(EOF·RST)도 오지 않고 대기열에 있다
    for client in clients:
        if client not in served:
            check(not client.has_data_now(),
                  "waiting client was refused or closed")
    ratio = measure_cpu_ratio(server, 0.5)
    check(ratio < IDLE_CPU_RATIO, "CPU %.0f%%" % (ratio * 100))


def collect_served(clients):
    served = []
    for client in clients:
        if getattr(client, "served", False):
            continue
        if client.has_data_now():
            response = client.read_response(1.0)
            check(response.status == 200, "status %d" % response.status)
            client.served = True
            served.append(client)
    return served


def check_waiting_served(server, clients):
    served = [client for client in clients if getattr(client, "served", False)]
    check(len(served) >= 10, "I-1 left too few served clients")
    for client in served[:10]:
        client.close()
        clients.remove(client)
    time.sleep(0.5)
    newly_served = collect_served(clients)
    check(len(newly_served) == 10,
          "%d waiting clients served, expected 10" % len(newly_served))


# ── J. 파일이 줄어듦 (L16) ──

def test_j1_file_shrinks(server, root):
    path = root.path("shrink.bin")
    client = Client(server.port, SMALL_RECEIVE_BUFFER)
    client.send(build_request("/shrink.bin"))
    response = client.read_head_only()
    promised = int(response.header("Content-Length"))
    received = len(client.buffer)
    client.buffer = b""
    while received < 100 * 1024:
        chunk = client.sock.recv(4096)
        check(chunk != b"", "closed before truncation")
        received += len(chunk)
        time.sleep(0.005)
    os.truncate(path, 1024 * 1024)
    data, how = client.read_until_close(10.0)
    received += len(data)
    check_closed(how)
    check(received <= promised, "received more than Content-Length")
    check(received < promised, "full length arrived after truncation")
    client.close()
    check_server_alive(server)


# ── K. 로그 (L15, L37) ──

def test_k1_log_format(server, root):
    client = Client(server.port)
    client.send(build_request("/a.html"))
    client.read_response()
    ok_port = client.local_port()
    client.close()
    payload_client, _ = open_413_while_sending(server)
    payload_port = payload_client.local_port()
    payload_client.close()
    reset_port = reset_during_big_download(server)
    time.sleep(LINGER_TOTAL + 0.5)

    ok_lines = find_log_lines(server, ok_port)
    check(len(ok_lines) == 1, "200: %d lines%s"
          % (len(ok_lines), describe_lines(ok_lines)))
    check(ok_lines[0].group(3) == "GET /a.html HTTP/1.1",
          "200: request line %r" % ok_lines[0].group(3))
    check(ok_lines[0].group(4) == "200", "200: status %s"
          % ok_lines[0].group(4))
    payload_lines = find_log_lines(server, payload_port)
    check(len(payload_lines) == 1, "413: %d lines%s"
          % (len(payload_lines), describe_lines(payload_lines)))
    check(payload_lines[0].group(4) == "413", "413: status %s"
          % payload_lines[0].group(4))
    reset_lines = find_log_lines(server, reset_port)
    check(len(reset_lines) == 1, "reset: %d lines%s"
          % (len(reset_lines), describe_lines(reset_lines)))
    check(reset_lines[0].group(7) == "reset", "reset: tag %r"
          % reset_lines[0].group(7))


def test_k2_no_access_log(binary, root):
    server = Server(binary, root.root, root.base, "nolog",
                    SHORT_TIMEOUT_OPTIONS + ["--no-access-log"])
    try:
        get(server.port, "/a.html")
        time.sleep(0.2)
        output = server.read_stdout()
    finally:
        server.stop()
    check(output == "", "stdout has %d bytes" % len(output))


def test_k3_idle_timeout_no_line(server, root):
    client = Client(server.port)
    client.send(build_request("/a.html"))
    client.read_response()
    port = client.local_port()
    _, how = client.read_until_close(KEEPALIVE_TIMEOUT + LATE_MARGIN + 1.0)
    check_closed(how)
    time.sleep(0.2)
    lines = find_log_lines(server, port)
    check(len(lines) == 1, "%d lines (only the 200 line expected)%s"
          % (len(lines), describe_lines(lines)))
    client.close()


# ── M. 자원 (Q5, L19, L36) ──

def test_m1_fds_and_cards(server, baseline_fds):
    time.sleep(LINGER_TOTAL + KEEPALIVE_TIMEOUT + 0.5)
    fds = server.count_fds()
    check(fds == baseline_fds, "fds %d, at start %d" % (fds, baseline_fds))
    counts = [COUNT_LOG_PATTERN.match(line)
              for line in server.read_stderr().splitlines()]
    counts = [match for match in counts if match is not None]
    check(counts, "no count line in stderr (--count-log)")
    created, destroyed = counts[-1].group(1), counts[-1].group(2)
    check(created == destroyed, "created %s, destroyed %s"
          % (created, destroyed))


def test_m2_no_bad_access(server):
    check(server.is_alive(), "server process exited")
    check("AddressSanitizer" not in server.read_stderr(),
          "AddressSanitizer report in stderr")


# ── 실행 순서 ──

MAIN_SERVER_TESTS = [
    ("A-1", test_a1_get_index),
    ("A-2", test_a2_one_megabyte_hash),
    ("A-3", test_a3_chunk_boundary),
    ("A-4", test_a4_empty_file),
    ("A-5", test_a5_static_errors_over_socket),
    ("B-1", test_b1_one_byte_at_a_time),
    ("B-2", test_b2_two_requests_in_one_send),
    ("B-3", test_b3_big_then_small),
    ("B-4", test_b4_second_request_split),
    ("C-1", test_c1_three_gets_same_connection),
    ("C-2", test_c2_connection_close),
    ("C-3", test_c3_keep_after_404_403_301),
    ("C-4", test_c4_static_400_closes),
    ("C-5", test_c5_parser_400_closes),
    ("D-1", test_d1_post_405_then_get),
    ("D-2", test_d2_get_with_body_then_get),
    ("D-3", test_d3_post_expect_closes),
    ("D-4", test_d4_body_stops_after_405),
    ("D-5", test_d5_body_trickles_after_405),
    ("E-1", test_e1_half_header_408),
    ("E-2", test_e2_slowloris_408),
    ("E-4", test_e4_idle_close_silently),
    ("E-5", test_e5_send_blocked_closes),
    ("E-6", test_e6_other_client_while_slow),
    ("F-1", test_f1_413_not_reset),
    ("F-2", test_f2_quiet_after_413),
    ("F-3", test_f3_keep_sending_after_413),
    ("G-1", test_g1_close_right_away),
    ("G-2", test_g2_half_header_then_close),
    ("G-3", test_g3_reset_during_download),
    ("G-4", test_g4_half_close_after_request),
    ("G-5", test_g5_reset_hundred_times),
    ("H-1", test_h1_fifty_concurrent),
    ("H-2", test_h2_slow_and_normal),
    ("H-3", test_h3_idle_keepalive_cpu),
    ("J-1", test_j1_file_shrinks),
    ("K-1", test_k1_log_format),
    ("K-3", test_k3_idle_timeout_no_line),
]


def main():
    if len(sys.argv) != 2:
        print("usage: %s <server binary>" % sys.argv[0], file=sys.stderr)
        return 2
    binary = os.path.abspath(sys.argv[1])
    root = StaticRoot()
    try:
        run_all(binary, root)
    finally:
        root.remove()
    print("통과 %d, 실패 %d" % (results["pass"], results["fail"]))
    if results["failed_ids"]:
        print("실패한 ID: " + ", ".join(results["failed_ids"]))
    print("E-3은 4단계로 넘김 (L21)")
    return 1 if results["fail"] else 0


def run_all(binary, root):
    server = Server(binary, root.root, root.base, "main",
                    SHORT_TIMEOUT_OPTIONS + ["--count-log"])
    try:
        time.sleep(0.3)
        baseline_fds = server.count_fds()
        for test_id, function in MAIN_SERVER_TESTS:
            run_test(test_id, function, server, root)
        run_test("M-1", test_m1_fds_and_cards, server, baseline_fds)
        run_test("M-2", test_m2_no_bad_access, server)
    finally:
        server.stop()
    test_i_connection_limit(binary, root)
    run_test("K-2", test_k2_no_access_log, binary, root)


if __name__ == "__main__":
    sys.exit(main())
