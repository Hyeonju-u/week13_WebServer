#!/usr/bin/env python3
"""멀티 이벤트 루프 테스트 ML-1~3 (docs/multiloop.md)."""
import os
import shutil
import signal
import socket
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import (BINARY, Checker, Process, PythonBackend, connect, find_free_port, get,
                     read_response, wait_for_port)

checker = Checker()
check = checker.check
LOOPS = 4


class LoopServer(Process):
    """--loops를 붙여 띄우는 서버. 시작 로그를 보려고 기본 로그 레벨은 info다."""

    def __init__(self, root, loops, config=None, max_connections=None):
        self.port = find_free_port()
        command = [os.environ.get("WEBSERVER_BINARY", BINARY), "--root", root,
                   "--port", str(self.port), "--log-level", "info", "--loops", str(loops)]
        if config:
            command += ["--config", config]
        if max_connections:
            command += ["--max-connections", str(max_connections)]
        super().__init__(command, "webserver-loops-%d-" % self.port, environment=os.environ.copy())
        wait_for_port(self.port)


def prepare_root(base):
    root = os.path.join(base, "root")
    os.makedirs(root)
    with open(os.path.join(root, "index.html"), "w") as handle:
        handle.write("multi loop")
    return root


def write_config(base, backend_port):
    path = os.path.join(base, "proxy.conf")
    with open(path, "w") as handle:
        handle.write("route /py 127.0.0.1:%d\n" % backend_port)
    return path


def send_request(sock, path):
    sock.sendall(("GET %s HTTP/1.1\r\nHost: t\r\n\r\n" % path).encode())


def test_start_and_serve(root, config):
    """ML-1 (M1, M2): --loops 4로 시작하면 로그에 루프 4개, 정적·프록시 요청 정상"""
    print("[ML-1 루프 4개로 시작]")
    server = LoopServer(root, LOOPS, config=config)
    check("ML-1 시작 로그에 루프 %d개" % LOOPS, ("루프 %d개" % LOOPS) in server.log_text(),
          server.log_text()[-300:])
    statics = [get(server.port, "/index.html", "Connection: close\r\n") for _ in range(40)]
    check("ML-1 정적 요청 40개 모두 200",
          all(r is not None and r.status == 200 and r.body == b"multi loop" for r in statics))
    proxied = [get(server.port, "/py/echo", "Connection: close\r\n") for _ in range(20)]
    check("ML-1 프록시 요청 20개 모두 200", all(r is not None and r.status == 200 for r in proxied))
    code = server.stop()
    check("ML-1 SIGINT로 정상 종료(코드 0)", code == 0, "code=%s" % code)


def test_connection_limit(root):
    """ML-2 (M4): 상한 4에 연결 5개면 4개만 받고, 하나를 닫으면 5번째를 받는다"""
    print("[ML-2 전체 연결 상한]")
    server = LoopServer(root, LOOPS, max_connections=4)
    held = []
    for _ in range(4):
        sock = connect(server.port)
        send_request(sock, "/index.html")
        response, _ = read_response(sock)
        check("ML-2 앞의 연결 4개는 응답을 받음", response is not None and response.status == 200)
        held.append(sock)
    fifth = connect(server.port)
    send_request(fifth, "/index.html")
    fifth.settimeout(1.5)
    try:
        early = fifth.recv(1)
    except socket.timeout:
        early = None
    check("ML-2 5번째 연결은 상한이 차 있는 동안 응답을 받지 못함", early is None, repr(early))
    held.pop().close()
    fifth.settimeout(5)
    response, _ = read_response(fifth)
    check("ML-2 하나를 닫으면 5번째 연결이 응답을 받음", response is not None and response.status == 200)
    for sock in held + [fifth]:
        sock.close()
    code = server.stop()
    check("ML-2 SIGINT로 정상 종료(코드 0)", code == 0, "code=%s" % code)


def test_graceful_shutdown(root, config):
    """ML-3 (M6): 처리 중 요청이 걸린 채 SIGINT를 받으면 모두 끝낸 뒤 7초 안에 코드 0으로 종료"""
    print("[ML-3 처리 중 요청이 있는 채 종료]")
    server = LoopServer(root, LOOPS, config=config)
    clients = []
    for _ in range(8):
        sock = connect(server.port)
        send_request(sock, "/py/slow?seconds=2")
        clients.append(sock)
    time.sleep(0.5)
    started = time.time()
    server.process.send_signal(signal.SIGINT)
    responses = []
    for sock in clients:
        response, _ = read_response(sock)
        responses.append(response)
        sock.close()
    code = server.process.wait(15)
    elapsed = time.time() - started
    check("ML-3 처리 중이던 요청 8개 모두 끝까지 응답 (200, late)",
          all(r is not None and r.status == 200 and r.body == b"late" for r in responses))
    check("ML-3 종료 코드 0", code == 0, "code=%s" % code)
    check("ML-3 신호 후 7초 안에 종료 (%.1f초)" % elapsed, elapsed < 7)


def main():
    base = tempfile.mkdtemp(prefix="ws_loops_")
    backend = PythonBackend()
    try:
        root = prepare_root(base)
        config = write_config(base, backend.port)
        test_start_and_serve(root, config)
        test_connection_limit(root)
        test_graceful_shutdown(root, config)
    finally:
        backend.stop()
        shutil.rmtree(base, ignore_errors=True)
    return checker.finish()


if __name__ == "__main__":
    sys.exit(main())
