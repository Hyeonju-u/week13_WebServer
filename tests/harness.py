"""기능·가혹 조건 테스트가 함께 쓰는 도구: 서버 실행, 원시 소켓 클라이언트, 측정."""
import hashlib
import os
import re
import resource
import signal
import socket
import subprocess
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BINARY = os.environ.get("WEBSERVER_BINARY", os.path.join(ROOT, "build", "webserver"))
BACKEND_B = os.path.join(ROOT, "tests", "backend_b.py")
LOCALHOST = "127.0.0.1"
IS_LINUX = sys.platform.startswith("linux")


def count_fds_linux(pid):
    try:
        return len(os.listdir("/proc/%d/fd" % pid))
    except OSError:
        return 0


def count_fds_macos(pid):
    result = subprocess.run(["lsof", "-p", str(pid)], capture_output=True, text=True)
    return max(len(result.stdout.splitlines()) - 1, 0)


def read_rss_kb_linux(pid):
    try:
        with open("/proc/%d/status" % pid) as handle:
            for line in handle:
                if line.startswith("VmRSS:"):
                    return int(line.split()[1])
    except OSError:
        pass
    return 0


def read_footprint_kb_macos(pid):
    result = subprocess.run(["footprint", "-p", str(pid)], capture_output=True, text=True)
    match = re.search(r"phys_footprint:\s+([\d.]+)\s+(KB|MB|GB)", result.stdout)
    if match:
        scale = {"KB": 1, "MB": 1024, "GB": 1024 * 1024}[match.group(2)]
        return int(float(match.group(1)) * scale)
    result = subprocess.run(["ps", "-o", "rss=", "-p", str(pid)], capture_output=True, text=True)
    return int(result.stdout.strip() or 0)


def find_free_port():
    with socket.socket() as probe:
        probe.bind((LOCALHOST, 0))
        return probe.getsockname()[1]


def fingerprint(data):
    return hashlib.sha256(data).hexdigest()


def limit_open_files(soft_limit, hard=False):
    """자식 프로세스의 fd 한도를 낮춘다. hard=False면 서버가 스스로 한도를 올리는지(CON-10),
    hard=True면 올릴 수 없는 한도에서 fd가 바닥나는 상황(CON-11)을 만든다."""
    def apply():
        upper = soft_limit if hard else resource.getrlimit(resource.RLIMIT_NOFILE)[1]
        resource.setrlimit(resource.RLIMIT_NOFILE, (soft_limit, upper))
    return apply


class Process:
    """PID 하나를 감시하는 공통 부분: fd 수, 메모리, 종료."""

    def __init__(self, command, log_name, fd_limit=None, environment=None, hard_limit=False):
        self.log_file = tempfile.NamedTemporaryFile("w+b", prefix=log_name, delete=False)
        self.process = subprocess.Popen(command, stderr=self.log_file, stdout=self.log_file,
                                        env=environment,
                                        preexec_fn=limit_open_files(fd_limit, hard_limit) if fd_limit else None)

    @property
    def pid(self):
        return self.process.pid

    @property
    def is_alive(self):
        return self.process.poll() is None

    def cpu_seconds(self):
        """지금까지 쓴 CPU 시간(초). 유휴 상태에서 루프가 헛도는지 확인하는 데 쓴다."""
        result = subprocess.run(["ps", "-o", "cputime=", "-p", str(self.pid)], capture_output=True, text=True)
        total = 0.0
        for part in result.stdout.strip().split(":"):
            total = total * 60 + float(part or 0)
        return total

    def fd_count(self):
        """Linux의 /proc 항목 수는 lsof보다 작다. lsof는 실행 파일·작업 폴더도 세기 때문이다."""
        if IS_LINUX:
            return count_fds_linux(self.pid)
        return count_fds_macos(self.pid)

    def memory_kb(self):
        """실제 점유 메모리(KB). macOS는 free한 페이지를 RSS에 계속 세므로 footprint를 쓰고,
        Linux는 해제한 페이지가 RSS에서 빠지므로 VmRSS를 쓴다."""
        if IS_LINUX:
            return read_rss_kb_linux(self.pid)
        return read_footprint_kb_macos(self.pid)

    def count_leaks(self):
        """macOS의 leaks 명령으로 누수 수를 센다. 쓸 수 없으면 None.
        Linux에는 살아 있는 프로세스를 검사하는 도구가 없어 None이다.
        Linux 누수는 make asan-test가 종료 때 LeakSanitizer 보고로 확인한다."""
        if IS_LINUX or os.environ.get("WEBSERVER_INSERT_LIBRARY"):
            return None
        result = subprocess.run(["leaks", str(self.pid)], capture_output=True, text=True)
        match = re.search(r"(\d+) leaks? for (\d+) total leaked bytes", result.stdout)
        return (int(match.group(1)), int(match.group(2))) if match else None

    def stop(self, signal_number=signal.SIGINT, timeout=10):
        if self.process.poll() is None:
            self.process.send_signal(signal_number)
            try:
                self.process.wait(timeout)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait()
        return self.process.returncode

    def log_text(self):
        self.log_file.flush()
        with open(self.log_file.name, "rb") as handle:
            return handle.read().decode("utf-8", "replace")


class Server(Process):
    def __init__(self, root, port=None, config=None, log_level="error",
                 max_connections=None, binary=None, fd_limit=None, hard_limit=False):
        self.port = port or find_free_port()
        command = [binary or BINARY, "--root", root, "--port", str(self.port),
                   "--log-level", log_level]
        if config:
            command += ["--config", config]
        if max_connections:
            command += ["--max-connections", str(max_connections)]
        environment = os.environ.copy()
        # 이 환경에서는 ASan이 시작 전에 멈춰서, macOS의 Guard Malloc을 대신 끼워 넣을 수 있게 했다
        if os.environ.get("WEBSERVER_INSERT_LIBRARY"):
            environment["DYLD_INSERT_LIBRARIES"] = os.environ["WEBSERVER_INSERT_LIBRARY"]
        super().__init__(command, "webserver-%d-" % self.port, fd_limit, environment, hard_limit)
        wait_for_port(self.port, timeout=30 if os.environ.get("WEBSERVER_INSERT_LIBRARY") else 8)


class PythonBackend(Process):
    def __init__(self, port=None):
        self.port = port or find_free_port()
        super().__init__(["python3", "-I", BACKEND_B, str(self.port)], "backend-%d-" % self.port)
        wait_for_port(self.port)


def wait_for_port(port, timeout=8):
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            with socket.create_connection((LOCALHOST, port), timeout=0.5):
                return
        except OSError:
            time.sleep(0.05)
    raise RuntimeError("포트 %d가 열리지 않음" % port)


def connect(port, timeout=10):
    sock = socket.create_connection((LOCALHOST, port), timeout=timeout)
    sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    return sock


class Response:
    def __init__(self, raw, status, reason, headers, body):
        self.raw = raw
        self.status = status
        self.reason = reason
        self.headers = headers
        self.body = body

    def header(self, name):
        values = [value for key, value in self.headers if key.lower() == name.lower()]
        return values[0] if values else None

    def header_values(self, name):
        return [value for key, value in self.headers if key.lower() == name.lower()]


def read_head(sock, pending=b""):
    data = pending
    while b"\r\n\r\n" not in data:
        chunk = sock.recv(65536)
        if not chunk:
            return None, data
        data += chunk
    head, _, rest = data.partition(b"\r\n\r\n")
    return head, rest


def parse_head(head):
    lines = head.decode("latin-1").split("\r\n")
    parts = lines[0].split(" ", 2)
    headers = []
    for line in lines[1:]:
        name, _, value = line.partition(":")
        headers.append((name.strip(), value.strip()))
    return int(parts[1]), (parts[2] if len(parts) > 2 else ""), headers


def read_response(sock, pending=b"", is_head=False):
    """응답 하나를 읽는다. (응답, 남은 바이트)를 돌려준다. 끊기면 응답은 None."""
    head, rest = read_head(sock, pending)
    if head is None:
        return None, rest
    status, reason, headers = parse_head(head)
    response = Response(head, status, reason, headers, b"")
    length = response.header("Content-Length")
    if is_head or status in (204, 304) or 100 <= status < 200:
        return response, rest
    if length is not None:
        length = int(length)
        body = rest
        while len(body) < length:
            chunk = sock.recv(min(1 << 20, length - len(body)))
            if not chunk:
                break
            body += chunk
        response.body = body[:length]
        return response, body[length:]
    body = rest
    while True:
        chunk = sock.recv(1 << 16)
        if not chunk:
            break
        body += chunk
    response.body = body
    return response, b""


def exchange(port, raw, is_head=False, timeout=10):
    """요청 하나를 보내고 응답 하나를 받은 뒤, 서버가 연결을 닫았는지도 알려 준다."""
    with connect(port, timeout) as sock:
        sock.sendall(raw)
        response, rest = read_response(sock, is_head=is_head)
        closed = False
        try:
            sock.settimeout(1.0)
            closed = sock.recv(1) == b""
        except socket.timeout:
            closed = False
        except OSError:
            closed = True
        return response, closed


def get(port, path, headers="", **kwargs):
    raw = ("GET %s HTTP/1.1\r\nHost: test\r\n%s\r\n" % (path, headers)).encode("latin-1")
    return exchange(port, raw, **kwargs)[0]


class Checker:
    def __init__(self):
        self.failures = []
        self.count = 0

    def check(self, name, condition, detail=""):
        self.count += 1
        if condition:
            print("  통과  %s" % name)
        else:
            print("  실패  %s %s" % (name, detail))
            self.failures.append(name)
        return condition

    def finish(self):
        print("\n%d개 중 %d개 실패" % (self.count, len(self.failures)))
        return 1 if self.failures else 0
