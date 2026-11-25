"""README의 주장을 하나씩 실제 서버에 재현해서 보여 주는 데모.
주장마다 실제로 보낸 요청(→), 서버가 실제로 보낸 응답(←), 시각, 서버 로그, 메모리 수치를
일어나는 그대로 출력하고, 같은 기록을 demo/results/proof/proof.json에 남긴다.
컨테이너 안에서 실행한다: demo/proof.sh [구역...]
구역: memory limits cleanup sigpipe shutdown concurrency proxy security attack (없으면 전부)"""
import json
import os
import resource
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BINARY = os.path.join(ROOT, "build", "webserver")
OUT = os.path.join(ROOT, "demo", "results", "proof")
WWW = "/tmp/proof-www"
LOCALHOST = "127.0.0.1"
CORES = os.cpu_count() or 1
RESULTS = []
CURRENT = {}
PRINT_LOCK = threading.Lock()
GREEN, RED, DIM, BOLD, CYAN, RESET = "\033[32m", "\033[31m", "\033[2m", "\033[1m", "\033[36m", "\033[0m"
SHOWN_HEADERS = ("content-type", "content-length", "connection", "location")


# ── 기록과 출력 ──

def show_section(title):
    print("\n%s━━ %s %s%s" % (BOLD, title, "━" * max(4, 50 - len(title) * 2), RESET), flush=True)


def begin(section, claim, how):
    CURRENT.clear()
    CURRENT.update({"section": section, "claim": claim, "how": how, "log": [], "started": time.time()})
    print("  %s주장%s  %s" % (BOLD, RESET, claim))
    print("  %s방법  %s%s" % (DIM, how, RESET), flush=True)


def say(line):
    """일어난 일 한 줄. 터미널에 바로 찍고, 주장을 시작한 뒤 몇 초에 일어났는지와 함께 기록한다
    (발표 덱이 같은 간격으로 다시 보여 준다)"""
    with PRINT_LOCK:
        CURRENT["log"].append({"t": round(time.time() - CURRENT["started"], 2), "text": line})
        print("    %s│%s %s" % (CYAN, RESET, line), flush=True)


def finish(evidence, ok):
    """ok: True 맞음, False 다름. 한계 항목은 '한계가 README 말대로 있음'이 True"""
    result = dict(CURRENT, evidence=evidence, ok=ok, seconds=round(time.time() - CURRENT["started"], 2))
    del result["started"]
    RESULTS.append(result)
    mark = "%s✔ 맞음%s" % (GREEN, RESET) if ok else "%s✘ 다름%s" % (RED, RESET)
    print("  %s결과%s  %s   %s\n" % (BOLD, RESET, evidence, mark), flush=True)


def shorten(text, limit=86):
    if len(text) <= limit:
        return text
    return "%s …(총 %s자)… %s" % (text[:40], format(len(text), ","), text[-16:])


def show_request(raw):
    head, _, body = raw.partition(b"\r\n\r\n")
    for line in head.decode("latin-1").split("\r\n"):
        say("→ " + shorten(line))
    if body:
        say("→ (본문 %s바이트)" % format(len(body), ","))


def show_response(data, with_body=False):
    if not data:
        say("← (응답 없이 연결이 끊김)")
        return
    head, _, body = data.partition(b"\r\n\r\n")
    lines = head.decode("latin-1").split("\r\n")
    say("← " + lines[0])
    for line in lines[1:]:
        if line.split(":")[0].strip().lower() in SHOWN_HEADERS:
            say("← " + line)
    if with_body and body:
        say("← " + shorten(body.decode("utf-8", "replace").strip().splitlines()[0]))


def stamp(start):
    return "[%5.1f초]" % (time.time() - start)


# ── 서버·백엔드 실행 ──

def find_free_port():
    with socket.socket() as probe:
        probe.bind((LOCALHOST, 0))
        return probe.getsockname()[1]


def wait_for_port(port, timeout=10):
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            with socket.create_connection((LOCALHOST, port), timeout=0.5):
                return
        except OSError:
            time.sleep(0.05)
    raise RuntimeError("포트 %d가 열리지 않음" % port)


class Server:
    def __init__(self, *extra, config_lines=None):
        self.port = find_free_port()
        self.command = [BINARY, "--root", WWW, "--port", str(self.port), "--log-level", "info"]
        if config_lines:
            config = tempfile.NamedTemporaryFile("w", suffix=".conf", delete=False)
            config.write("\n".join(config_lines) + "\n")
            config.close()
            self.command += ["--config", config.name]
            self.config_lines = config_lines
        self.command += list(extra)
        self.log = tempfile.NamedTemporaryFile("w+b", delete=False)
        self.process = subprocess.Popen(self.command, stdout=self.log, stderr=self.log)
        wait_for_port(self.port)

    def describe(self):
        """서버를 어떻게 띄웠는지 기록에 남긴다"""
        shown = ["webserver" if part == BINARY else part for part in self.command]
        shown = ["www" if part == WWW else part for part in shown]
        say("$ " + " ".join(part if not part.endswith(".conf") else "proxy.conf" for part in shown))
        for line in getattr(self, "config_lines", []):
            say("  proxy.conf: " + line)

    def rss_kb(self):
        with open("/proc/%d/status" % self.process.pid) as handle:
            for line in handle:
                if line.startswith("VmRSS:"):
                    return int(line.split()[1])
        return 0

    def fd_count(self):
        return len(os.listdir("/proc/%d/fd" % self.process.pid))

    def is_alive(self):
        return self.process.poll() is None

    def stop(self, signal_number=signal.SIGINT, timeout=15):
        if self.process.poll() is None:
            self.process.send_signal(signal_number)
            try:
                self.process.wait(timeout)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait()
        return self.process.returncode

    def log_lines(self):
        with open(self.log.name, "rb") as handle:
            return [line for line in handle.read().decode("utf-8", "replace").splitlines() if line.strip()]


def start_backend(script, port):
    process = subprocess.Popen(["python3", "-I", script, str(port)],
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    wait_for_port(port)
    return process


# ── 요청 보내기 (보낸 것과 받은 것을 그대로 기록) ──

def read_head(sock):
    data = b""
    while b"\r\n\r\n" not in data:
        chunk = sock.recv(65536)
        if not chunk:
            break
        data += chunk
    return data


def status_of(data):
    try:
        return int(data.split(b" ", 2)[1])
    except (IndexError, ValueError):
        return None


def send_raw(port, raw, timeout=5):
    """요청을 그대로 보내고 응답 머리를 기록한다. 상태 코드를 돌려준다 (응답 없으면 None)"""
    show_request(raw)
    with socket.create_connection((LOCALHOST, port), timeout=timeout) as sock:
        try:
            sock.sendall(raw)
            data = read_head(sock)
        except (socket.timeout, ConnectionResetError, BrokenPipeError):
            data = b""
    show_response(data)
    return status_of(data)


def fetch(port, path, headers="", show="full"):
    """새 연결로 GET 하나. (상태 코드, 본문). show: full, brief(한 줄), none"""
    raw = ("GET %s HTTP/1.1\r\nHost: demo\r\nConnection: close\r\n%s\r\n" % (path, headers)).encode()
    if show == "full":
        show_request(raw)
    with socket.create_connection((LOCALHOST, port), timeout=10) as sock:
        sock.sendall(raw)
        data = b""
        while True:
            chunk = sock.recv(65536)
            if not chunk:
                break
            data += chunk
    status = status_of(data)
    body = data.partition(b"\r\n\r\n")[2].decode("utf-8", "replace").strip()
    if show == "full":
        show_response(data, with_body=True)
    elif show == "brief":
        say("→ GET %s   ← %s %s" % (path, status, shorten(body.splitlines()[0]) if body else ""))
    return status, body


def prepare_www():
    os.makedirs(WWW, exist_ok=True)
    with open(os.path.join(WWW, "index.html"), "w") as handle:
        handle.write("<html><body>hello</body></html>\n")
    with open(os.path.join(WWW, ".env"), "w") as handle:
        handle.write("SECRET=demo\n")
    with open(os.path.join(WWW, "big.bin"), "wb") as handle:
        handle.truncate(50 * 1024 * 1024)
    link = os.path.join(WWW, "link")
    if not os.path.islink(link):
        os.symlink("/etc/passwd", link)


# ── ① 메모리 ──

def prove_memory():
    show_section("① 메모리: 한 명당 자원을 적게")
    begin("memory", "연결 하나의 구조체는 320B", "src/connection.h를 포함해 sizeof(conn_t)를 출력하는 C 파일을 컴파일해 실행")
    source = "/tmp/proof_size.c"
    with open(source, "w") as handle:
        handle.write('#include <stdio.h>\n#include "connection.h"\n'
                     'int main (void) { printf("%zu", sizeof(conn_t)); return 0; }\n')
    say("$ cat size.c")
    say('  int main (void) { printf("%zu", sizeof(conn_t)); }')
    subprocess.run(["cc", "-I", os.path.join(ROOT, "src"), source, "-o", "/tmp/proof_size"], check=True)
    say("$ cc -I src size.c -o size && ./size")
    size = int(subprocess.run(["/tmp/proof_size"], capture_output=True, text=True).stdout)
    say("  %d" % size)
    finish("%d 바이트" % size, size == 320)

    count = 5000
    server = Server("--max-connections", "20000")
    sockets = []
    try:
        begin("memory", "버퍼는 필요할 때만 만든다 (가만히 있는 손님은 버퍼 없음)",
              "손님 %s명이 연결만 하고 아무것도 안 보낼 때 서버 메모리(/proc의 VmRSS) 변화" % format(count, ","))
        server.describe()
        time.sleep(0.5)
        before = server.rss_kb()
        say("서버 VmRSS %s kB   (손님 0명)" % format(before, ","))
        for _ in range(count):
            sockets.append(socket.create_connection((LOCALHOST, server.port)))
        say("→ 손님 %s명 연결, 아무것도 안 보냄" % format(count, ","))
        time.sleep(1.5)
        idle = server.rss_kb()
        per_idle = (idle - before) * 1024 / count
        say("서버 VmRSS %s kB   (+%s kB ÷ %s명 = 1명당 %d바이트)" % (
            format(idle, ","), format(idle - before, ","), format(count, ","), per_idle))
        finish("1명당 %d 바이트" % per_idle, per_idle < 1024)

        begin("memory", "요청이 들어오면 그때 4KB 버퍼를 만든다",
              "같은 %s명이 요청 첫 줄만 보냈을 때 서버 메모리 변화" % format(count, ","))
        for sock in sockets:
            sock.sendall(b"GET /index.html HTTP/1.1\r\n")
        say("→ %s명 모두 'GET /index.html HTTP/1.1' 한 줄만 보냄 (헤더는 아직)" % format(count, ","))
        time.sleep(1.5)
        reading = server.rss_kb()
        per_reading = (reading - idle) * 1024 / count
        say("서버 VmRSS %s kB   (+%s kB ÷ %s명 = 1명당 %.1f KB)" % (
            format(reading, ","), format(reading - idle, ","), format(count, ","), per_reading / 1024))
        finish("1명당 %.1f KB 더 씀" % (per_reading / 1024), 3500 < per_reading < 6000)
    finally:
        for sock in sockets:
            sock.close()
        server.stop()


# ── 상한 ──

def prove_limits():
    show_section("① 메모리: 요청 크기 상한")
    server = Server()
    try:
        cases = [
            ("요청 줄 8KB 넘으면 414", "URI가 9,000자인 GET",
             b"GET /" + b"a" * 9000 + b" HTTP/1.1\r\nHost: demo\r\n\r\n", 414),
            ("헤더 16KB 넘으면 431", "17,000바이트짜리 헤더 하나",
             b"GET / HTTP/1.1\r\nHost: demo\r\nX-Big: " + b"a" * 17000 + b"\r\n\r\n", 431),
            ("본문 1MB 넘으면 413", "Content-Length: 2,000,000인 POST (본문은 안 보냄)",
             b"POST /index.html HTTP/1.1\r\nHost: demo\r\nContent-Length: 2000000\r\n\r\n", 413),
        ]
        for claim, how, raw, expected in cases:
            begin("limits", claim, how)
            status = send_raw(server.port, raw)
            finish("응답 %s" % status, status == expected)
    finally:
        server.stop()


# ── 회수 (타이머) ──

def wait_closed(sock):
    sock.settimeout(30)
    try:
        while sock.recv(65536):
            pass
    except OSError:
        pass


def prove_cleanup():
    show_section("① 메모리: 끝난 연결은 바로 회수")
    server = Server()
    try:
        begin("cleanup", "아무것도 안 보내는 손님은 15초 뒤 정리", "연결만 하고 가만히 있기")
        start = time.time()
        sock = socket.create_connection((LOCALHOST, server.port))
        say("%s → 연결함. 아무것도 안 보내고 기다림" % stamp(start))
        wait_closed(sock)
        silent = time.time() - start
        say("%s ← 서버가 연결을 끊음" % stamp(start))
        finish("%.1f초 뒤 끊김" % silent, 14 <= silent <= 17)

        begin("cleanup", "응답을 받고 쉬는(keep-alive) 손님은 5초 뒤 정리", "요청 하나 보내고 응답을 받은 뒤 가만히 있기")
        sock = socket.create_connection((LOCALHOST, server.port))
        start = time.time()
        sock.sendall(b"GET /index.html HTTP/1.1\r\nHost: demo\r\n\r\n")
        say("%s → GET /index.html (keep-alive)" % stamp(start))
        data = read_head(sock)
        say("%s ← %s" % (stamp(start), data.split(b"\r\n")[0].decode()))
        start = time.time()
        say("%s   응답을 받았고, 이제 아무것도 안 보냄" % stamp(start))
        wait_closed(sock)
        idle = time.time() - start
        say("%s ← 서버가 연결을 끊음" % stamp(start))
        finish("%.1f초 뒤 끊김" % idle, 4 <= idle <= 7)

        begin("security", "Slowloris: 요청 읽기는 총 15초 (헤더를 조금씩 보내도 갱신 안 함)",
              "헤더를 1초에 한 줄씩 끝없이 보내기")
        sock = socket.create_connection((LOCALHOST, server.port))
        start = time.time()
        sock.sendall(b"GET /index.html HTTP/1.1\r\nHost: demo\r\n")
        say("%s → GET /index.html HTTP/1.1  (빈 줄은 영원히 안 보냄)" % stamp(start))
        sent = 0
        try:
            for i in range(40):
                time.sleep(1)
                sock.sendall(b"X-Slow-%d: a\r\n" % i)
                sent += 1
                if i < 2 or i % 5 == 4:
                    say("%s → X-Slow-%d: a" % (stamp(start), i))
        except OSError:
            pass
        closed = time.time() - start
        sock.settimeout(1)
        try:
            reply = sock.recv(4096).split(b"\r\n")[0].decode()
        except OSError:
            reply = ""
        say("%s ← 서버가 연결을 끊음%s (헤더 %d줄 보낸 뒤)" % (
            stamp(start), " (%s)" % reply if reply else "", sent))
        finish("%.1f초 뒤 끊김" % closed, 14 <= closed <= 17.5)
    finally:
        server.stop()


# ── SIGPIPE ──

def prove_sigpipe():
    show_section("① 메모리: 손님이 갑자기 끊어도 서버는 유지")
    server = Server()
    try:
        begin("sigpipe", "쓰는 도중 손님이 끊어도(SIGPIPE 무시) 그 연결만 닫고 서버는 유지",
              "50MB 파일을 받다가 1KB만 읽고 강제로 끊기(RST)를 30번 → 다른 요청")
        baseline = server.fd_count()
        say("서버 PID %d, 열린 fd %d개" % (server.process.pid, baseline))
        for i in range(30):
            sock = socket.create_connection((LOCALHOST, server.port))
            sock.sendall(b"GET /big.bin HTTP/1.1\r\nHost: demo\r\n\r\n")
            received = len(sock.recv(1000))
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, b"\x01\x00\x00\x00\x00\x00\x00\x00")
            sock.close()
            if i in (0, 29):
                say("→ GET /big.bin, %s바이트 받고 RST로 끊음  (%d번째)" % (format(received, ","), i + 1))
            elif i == 1:
                say("  … 같은 일을 30번")
        time.sleep(1)
        alive = server.is_alive()
        say("서버 PID %d %s, 열린 fd %d개" % (server.process.pid, "살아 있음" if alive else "죽음",
                                         server.fd_count()))
        status, _ = fetch(server.port, "/index.html", show="brief")
        after = server.fd_count()
        finish("서버 유지, 다음 요청 %s, fd %d → %d개" % (status, baseline, after),
               alive and status == 200 and after <= baseline)
    finally:
        server.stop()


# ── 종료 ──

def prove_shutdown():
    show_section("① 메모리: SIGINT 정상 종료")
    backend_port = find_free_port()
    backend = start_backend(os.path.join(ROOT, "tests", "backend_b.py"), backend_port)
    server = Server(config_lines=["route /py 127.0.0.1:%d" % backend_port])
    outcome = {}
    start = time.time()

    def slow_request():
        outcome["status"], outcome["body"] = fetch(server.port, "/py/slow?seconds=3", show="none")
        outcome["at"] = stamp(start)

    try:
        begin("shutdown", "SIGINT을 받으면 새 연결은 거절하고, 처리 중이던 응답은 끝까지 보내고 종료",
              "3초 걸리는 프록시 요청을 보내 두고 SIGINT → 새로 접속 → 처리 중이던 응답 확인")
        server.describe()
        start = time.time()
        thread = threading.Thread(target=slow_request)
        thread.start()
        say("%s → GET /py/slow?seconds=3  (백엔드가 3초 뒤 응답)" % stamp(start))
        time.sleep(0.5)
        server.process.send_signal(signal.SIGINT)
        say("%s $ kill -INT %d" % (stamp(start), server.process.pid))
        time.sleep(1.2)
        try:
            socket.create_connection((LOCALHOST, server.port), timeout=1).close()
            refused = False
        except OSError:
            refused = True
        say("%s → 새 손님 접속 시도   ← %s" % (stamp(start), "연결 거절 (ECONNREFUSED)" if refused else "받아 줌"))
        thread.join()
        say("%s ← 처리 중이던 요청: %s %s" % (outcome.get("at"), outcome.get("status"), outcome.get("body", "")[:30]))
        code = server.stop()
        say("%s   서버 프로세스 끝남, 종료 코드 %s" % (stamp(start), code))
        lines = server.log_lines()
        for text in dict.fromkeys(line for line in lines if "종료" in line):
            count = lines.count(text)
            say("  서버 로그: %s%s" % (text, "  (×%d, 루프마다 한 줄)" % count if count > 1 else ""))
        finish("새 접속 %s, 처리 중이던 응답 %s, 종료 코드 %s" % (
            "거절" if refused else "받음", outcome.get("status"), code),
            refused and outcome.get("status") == 200 and code == 0)
    finally:
        server.stop()
        backend.kill()


# ── ② 동시성 ──

def prove_concurrency():
    show_section("② 동시성: 멀티 이벤트 루프")
    server = Server()
    try:
        begin("concurrency", "루프 수 = 컴퓨터 코어 수 (시작할 때 읽어서 결정)", "--loops 없이 띄운 서버의 시작 로그와 스레드 수")
        say("$ nproc")
        say("  %d" % CORES)
        server.describe()
        start_line = next((line for line in server.log_lines() if "루프" in line), "")
        say("  서버 로그: " + start_line)
        loops = int(start_line.split("루프 ")[1].split("개")[0]) if "루프 " in start_line else None
        threads = len(os.listdir("/proc/%d/task" % server.process.pid))
        say("$ ls /proc/%d/task | wc -l" % server.process.pid)
        say("  %d   (루프 %s개 + 메인 1개)" % (threads, loops))
        finish("코어 %d개 → 루프 %s개" % (CORES, loops), loops == CORES)
    finally:
        server.stop()

    server = Server("--max-connections", "4")
    holders = []
    try:
        begin("concurrency", "연결 수는 모든 루프가 같이 쓰는 값 하나 (원자 연산)",
              "루프 %d개 서버를 최대 연결 4로 띄우고 4명 연결 → 5번째 손님 → 1명 나감" % CORES)
        server.describe()
        start = time.time()
        for i in range(4):
            sock = socket.create_connection((LOCALHOST, server.port))
            sock.sendall(b"GET /index.html HTTP/1.1\r\nHost: demo\r\n\r\n")
            status = status_of(read_head(sock))
            holders.append(sock)
            say("%s 손님 %d → GET /index.html   ← %s (연결 유지)" % (stamp(start), i + 1, status))
        fifth = socket.create_connection((LOCALHOST, server.port))
        fifth.sendall(b"GET /index.html HTTP/1.1\r\nHost: demo\r\nConnection: close\r\n\r\n")
        say("%s 손님 5 → GET /index.html" % stamp(start))
        fifth.settimeout(1.5)
        try:
            blocked = status_of(read_head(fifth)) is None
        except socket.timeout:
            blocked = True
        say("%s 손님 5   %s" % (stamp(start), "1.5초 동안 응답 없음 (커널 대기줄에서 기다리는 중)" if blocked else "바로 응답받음"))
        holders.pop().close()
        say("%s 손님 4가 나감" % stamp(start))
        fifth.settimeout(5)
        later = status_of(read_head(fifth))
        say("%s 손님 5   ← %s" % (stamp(start), later))
        fifth.close()
        finish("5번째는 기다렸다가 자리가 나자 %s" % later, blocked and later == 200)
    finally:
        for sock in holders:
            sock.close()
        server.stop()


# ── ③ 리버스 프록시 ──

def name_of(body, names):
    port = body.split("|")[0].replace("backend", "").strip() if body.startswith("backend") else ""
    return names.get(port, "-")


def prove_proxy():
    show_section("③ 리버스 프록시")
    script = os.path.join(ROOT, "demo", "name_backend.py")
    ports = [find_free_port() for _ in range(3)]
    names = {str(port): "B%d" % (i + 1) for i, port in enumerate(ports)}
    backends = [start_backend(script, port) for port in ports]
    server = Server(config_lines=["route /api %s" % " ".join("127.0.0.1:%d" % port for port in ports),
                                  "backend_fail_timeout 10"])
    try:
        begin("proxy", "경로 규칙별 Round-Robin (루프가 여럿이어도 차례 하나)",
              "백엔드 B1·B2·B3(자기 포트를 답함)을 /api에 두고 매번 새 연결로 6번 요청 (루프 %d개)" % CORES)
        server.describe()
        say("  B1=%d, B2=%d, B3=%d" % tuple(ports))
        order = [name_of(fetch(server.port, "/api/hello", show="brief")[1], names) for _ in range(6)]
        finish(" → ".join(order), len(set(order[:3])) == 3 and order[3:] == order[:3])

        begin("security", "X-Forwarded-For 위조는 실제 주소로 덮어씀",
              "X-Forwarded-For: 6.6.6.6을 넣어 /api 요청 → 백엔드가 실제로 받은 값")
        _, body = fetch(server.port, "/api/who", "X-Forwarded-For: 6.6.6.6\r\n")
        seen = body.split("X-Forwarded-For:")[1].strip() if "X-Forwarded-For:" in body else "-"
        finish("백엔드가 받은 값 %s" % seen, "6.6.6.6" not in seen)

        begin("proxy", "연결 실패한 백엔드는 10초 동안 뺌", "B2를 죽이고 6번 요청")
        backends[1].kill()
        backends[1].wait()
        say("$ kill B2 (포트 %d)" % ports[1])
        after = [fetch(server.port, "/api/hello", show="brief") for _ in range(6)]
        statuses = [status for status, _ in after]
        used = [name_of(body, names) for _, body in after]
        finish("상태 %s, 응답한 백엔드 %s" % (" ".join(map(str, statuses)), " ".join(used)),
               "B2" not in used and statuses.count(200) >= 5)

        begin("proxy", "백엔드가 전부 죽으면 502, 정적 파일은 프록시를 거치지 않음", "백엔드 셋 다 죽이고 /api와 /index.html 요청")
        for backend in backends:
            backend.kill()
        say("$ kill B1 B3")
        status_all_dead, _ = fetch(server.port, "/api/hello", show="brief")
        status_static, _ = fetch(server.port, "/index.html", show="brief")
        finish("/api → %s, /index.html → %s" % (status_all_dead, status_static),
               status_all_dead == 502 and status_static == 200)
    finally:
        server.stop()
        for backend in backends:
            backend.kill()

    backend_port = find_free_port()
    backend = start_backend(os.path.join(ROOT, "tests", "backend_b.py"), backend_port)
    server = Server(config_lines=["route /py 127.0.0.1:%d" % backend_port])
    try:
        begin("proxy", "배압: 손님이 느리면 64KB에서 백엔드 읽기를 멈춤 (메모리 보호)",
              "백엔드가 300MB를 보내는데 손님은 64KB만 읽고 멈춤 → 서버 메모리 변화")
        before = server.rss_kb()
        say("서버 VmRSS %s kB" % format(before, ","))
        sock = socket.create_connection((LOCALHOST, server.port))
        sock.sendall(b"GET /py/big?mb=300 HTTP/1.1\r\nHost: demo\r\n\r\n")
        say("→ GET /py/big?mb=300   (백엔드가 300MB를 보냄)")
        received = 0
        while received < 64 * 1024:
            received += len(sock.recv(65536))
        say("← %s바이트 받고 읽기를 멈춤" % format(received, ","))
        for second in (1, 2, 3, 4):
            time.sleep(1)
            say("  %d초 뒤 서버 VmRSS %s kB" % (second, format(server.rss_kb(), ",")))
        during = server.rss_kb()
        sock.close()
        finish("%.1f MB 늘어남 (쌓아 뒀다면 300MB)" % ((during - before) / 1024), during - before < 10 * 1024)
    finally:
        server.stop()
        backend.kill()


# ── ④ 보안 ──

def prove_security():
    show_section("④ 보안: 악의적인 요청")
    server = Server()
    try:
        cases = [
            ("../ 경로 이동은 404", "GET /../etc/passwd (curl처럼 정리하지 않고 그대로)", "/../etc/passwd", 404),
            ("숨김 파일 .env는 404", "www/.env를 만들어 두고 GET /.env", "/.env", 404),
            ("%2F 우회는 400", "GET /a%2Fb", "/a%2Fb", 400),
            ("%00 우회는 400", "GET /index.html%00.png", "/index.html%00.png", 400),
            ("심볼릭 링크는 403", "www/link → /etc/passwd를 만들어 두고 GET /link", "/link", 403),
        ]
        for claim, how, path, expected in cases:
            begin("security", claim, how)
            status = send_raw(server.port, ("GET %s HTTP/1.1\r\nHost: demo\r\n\r\n" % path).encode())
            finish("응답 %s" % status, status == expected)
        begin("security", "요청 스머글링: Transfer-Encoding + Content-Length 동시면 400", "두 헤더를 함께 보낸 POST")
        status = send_raw(server.port, b"POST /index.html HTTP/1.1\r\nHost: demo\r\n"
                          b"Transfer-Encoding: chunked\r\nContent-Length: 5\r\n\r\n0\r\n\r\n")
        finish("응답 %s" % status, status == 400)
    finally:
        server.stop()


# ── 외부 공격 도구 ──

def read_slowhttptest_csv(prefix):
    rows = []
    with open(prefix + ".csv") as handle:
        header = handle.readline().strip().split(",")
        for line in handle:
            values = line.strip().split(",")
            if len(values) == len(header):
                rows.append(dict(zip(header, values)))
    return rows


def run_slowhttptest(name, arguments, port):
    prefix = os.path.join(OUT, name)
    shown = " ".join(arguments).replace("http://127.0.0.1:%d" % port, "http://서버")
    say("$ slowhttptest %s" % shown)
    subprocess.run(["slowhttptest", *arguments, "-g", "-o", prefix],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    rows = read_slowhttptest_csv(prefix)
    # slowhttptest CSV의 Service Available은 서비스가 되면 -c 값, 안 되면 0이다
    for row in rows:
        second = int(row["Seconds"])
        if second % 5 == 0 or second == int(rows[-1]["Seconds"]):
            say("  %3d초  붙어 있음 %4s  서버가 끊음 %4s  정상 요청 %s" % (
                second, row["Connected"], row["Closed"], "응답함" if row["Service Available"] != "0" else "응답 없음"))
    return rows


def prove_attack():
    show_section("④ 보안: 외부 도구 slowhttptest")
    server = Server()
    try:
        begin("attack", "slowhttptest Slowloris 1,000명: 약 15초에 정리, 서비스 유지",
              "slowhttptest -H: 1,000명이 헤더를 5초마다 조금씩 보냄. 최대 40초 (공격 연결이 다 끊기면 일찍 끝남)")
        rows = run_slowhttptest("slowloris", [
            "-c", "1000", "-H", "-i", "5", "-r", "200", "-t", "GET",
            "-u", "http://127.0.0.1:%d/index.html" % server.port, "-x", "24", "-p", "3", "-l", "40"], server.port)
        down = [row for row in rows if row["Service Available"] == "0"]
        peak = max((int(row["Connected"]) for row in rows), default=0)
        closed = max((int(row["Closed"]) for row in rows), default=0)
        first_closed = next((row["Seconds"] for row in rows if int(row["Closed"]) > 0), "-")
        finish("최대 동시 %d명, %s초부터 끊기 시작해 %d개 정리, 서비스가 안 된 초 %d번" % (
            peak, first_closed, closed, len(down)), not down and closed > 0)

        begin("attack", "한계: Slow Read(응답을 아주 느리게 받는 손님)는 정리되지 않음",
              "slowhttptest -X: 500명이 50MB 파일 응답을 5초에 32바이트씩 읽음. 40초")
        rows = run_slowhttptest("slowread", [
            "-c", "500", "-X", "-r", "200", "-w", "512", "-y", "1024", "-n", "5", "-z", "32", "-k", "3",
            "-u", "http://127.0.0.1:%d/big.bin" % server.port, "-p", "3", "-l", "40"], server.port)
        last = rows[-1] if rows else {}
        finish("40초 뒤에도 %s명 남음, 서버가 끊은 연결 %s개" % (last.get("Connected", "?"), last.get("Closed", "?")),
               int(last.get("Connected", 0) or 0) > 0)
    finally:
        server.stop()


SECTIONS = {
    "memory": prove_memory, "limits": prove_limits, "cleanup": prove_cleanup,
    "sigpipe": prove_sigpipe, "shutdown": prove_shutdown, "concurrency": prove_concurrency,
    "proxy": prove_proxy, "security": prove_security, "attack": prove_attack,
}


def save_results(names):
    """일부 구역만 다시 돌리면 그 구역 결과만 바꿔 넣는다"""
    path = os.path.join(OUT, "proof.json")
    kept = []
    if os.path.exists(path):
        with open(path, encoding="utf-8") as handle:
            kept = [result for result in json.load(handle)["results"] if result.get("run") not in names]
    merged = kept + RESULTS
    merged.sort(key=lambda result: list(SECTIONS).index(result["run"]))
    with open(path, "w", encoding="utf-8") as handle:
        json.dump({"cores": CORES, "results": merged}, handle, ensure_ascii=False, indent=1)


def main():
    names = sys.argv[1:] or list(SECTIONS)
    soft, hard = resource.getrlimit(resource.RLIMIT_NOFILE)
    resource.setrlimit(resource.RLIMIT_NOFILE, (hard, hard))
    os.makedirs(OUT, exist_ok=True)
    prepare_www()
    for name in names:
        start = len(RESULTS)
        SECTIONS[name]()
        for result in RESULTS[start:]:
            result["run"] = name
    passed = sum(1 for result in RESULTS if result["ok"])
    print("%s%d개 중 %d개 README 말대로%s" % (BOLD, len(RESULTS), passed, RESET))
    save_results(names)
    return 0 if passed == len(RESULTS) else 1


if __name__ == "__main__":
    sys.exit(main())
