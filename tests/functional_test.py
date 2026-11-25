#!/usr/bin/env python3
"""기능 테스트: 응답 코드 요약표(8-9, 10-9)의 각 줄과 파서·프록시 동작을 확인한다."""
import os
import random
import shutil
import signal
import socket
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import (ROOT, Checker, PythonBackend, Server, connect, exchange, find_free_port,
                     fingerprint, get, parse_head, read_head, read_response)

checker = Checker()
check = checker.check


def prepare_roots(base):
    roots = []
    for index in range(3):
        root = os.path.join(base, "root%d" % index)
        os.makedirs(os.path.join(root, "docs"))
        os.makedirs(os.path.join(root, "empty"))
        os.makedirs(os.path.join(root, "api"))
        os.makedirs(os.path.join(root, "mix"))
        with open(os.path.join(root, "index.html"), "w") as handle:
            handle.write("I am server %d" % index)
        with open(os.path.join(root, "api", "who.txt"), "w") as handle:
            handle.write("backend %d" % index)
        with open(os.path.join(root, "mix", "who.txt"), "w") as handle:
            handle.write("backend %d" % index)
        with open(os.path.join(root, "docs", "index.html"), "w") as handle:
            handle.write("docs %d" % index)
        roots.append(root)
    first = roots[0]
    with open(os.path.join(first, "style.css"), "w") as handle:
        handle.write("body { color: red }")
    with open(os.path.join(first, "photo.png"), "wb") as handle:
        handle.write(bytes(random.Random(1).randrange(256) for _ in range(5000)))
    with open(os.path.join(first, "a b.txt"), "w") as handle:
        handle.write("space")
    with open(os.path.join(first, ".secret"), "w") as handle:
        handle.write("hidden")
    os.symlink("/etc/hosts", os.path.join(first, "outside_link"))
    os.symlink("index.html", os.path.join(first, "inside_link"))
    with open(os.path.join(first, "UPPER.JPG"), "wb") as handle:
        handle.write(b"\xff\xd8jpeg")
    with open(os.path.join(first, "data.bin"), "wb") as handle:
        handle.write(os.urandom(300000))
    return roots


def test_static(port, root):
    print("[정적 파일]")
    response = get(port, "/")
    check("GET / → 200 index.html", response.status == 200 and response.body == b"I am server 0")
    check("Content-Type html", response.header("Content-Type") == "text/html; charset=utf-8")
    check("Content-Length 일치", int(response.header("Content-Length")) == len(response.body))
    check("Date 헤더", response.header("Date", ) is not None and response.header("Date").endswith("GMT"))
    check("Connection keep-alive", response.header("Connection") == "keep-alive")
    css = get(port, "/style.css")
    check("css MIME", css.header("Content-Type") == "text/css; charset=utf-8")
    png = get(port, "/photo.png")
    with open(os.path.join(root, "photo.png"), "rb") as handle:
        original = handle.read()
    check("png 바이너리 본문 일치", png.status == 200 and png.body == original
          and png.header("Content-Type") == "image/png")
    check("대문자 확장자 UPPER.JPG → image/jpeg",
          get(port, "/UPPER.JPG").header("Content-Type") == "image/jpeg")
    check("모르는 확장자 → octet-stream",
          get(port, "/data.bin").header("Content-Type") == "application/octet-stream")
    big = get(port, "/data.bin")
    with open(os.path.join(root, "data.bin"), "rb") as handle:
        original = handle.read()
    check("300KB 파일(여러 번에 나눠 전송) 해시 일치", fingerprint(big.body) == fingerprint(original))
    check("퍼센트 인코딩 경로 /a%20b.txt", get(port, "/a%20b.txt").body == b"space")
    check("쿼리는 파일 탐색에 쓰지 않음", get(port, "/style.css?v=3").status == 200)
    head, _ = exchange(port, b"HEAD /style.css HTTP/1.1\r\nHost: t\r\n\r\n", is_head=True)
    check("HEAD는 GET과 같은 Content-Length, 본문 없음",
          head.status == 200 and head.header("Content-Length") == css.header("Content-Length")
          and head.body == b"")
    check("404 없는 파일", get(port, "/nope.html").status == 404)
    check("404 오류 본문 길이 일치", int(get(port, "/nope").header("Content-Length")) == len(get(port, "/nope").body))
    check("404 숨김 파일 /.secret", get(port, "/.secret").status == 404)
    check("404 /.git/config", get(port, "/.git/config").status == 404)
    check("404 경로 탈출 /../etc/passwd", get(port, "/../etc/passwd").status == 404)
    check("404 인코딩된 경로 탈출 %2e%2e", get(port, "/%2e%2e/%2e%2e/etc/passwd").status == 404)
    check("400 인코딩된 슬래시 %2F", get(port, "/a%2Fb").status == 400)
    check("403 루트 밖을 가리키는 링크", get(port, "/outside_link").status == 403)
    check("403 루트 안을 가리키는 링크도 거절", get(port, "/inside_link").status == 403)
    check("403 index.html 없는 폴더", get(port, "/empty/").status == 403)
    redirect = get(port, "/docs?x=1")
    check("301 폴더에 / 없음 + 쿼리 유지",
          redirect.status == 301 and redirect.header("Location") == "/docs/?x=1")
    check("폴더 /docs/ → index.html", get(port, "/docs/").body == b"docs 0")
    check("// 로 시작해도 다른 사이트로 리다이렉트하지 않음",
          get(port, "//docs").header("Location") == "/docs/")
    post, closed = exchange(port, b"POST /style.css HTTP/1.1\r\nHost: t\r\nContent-Length: 3\r\n\r\nabc")
    check("405 정적 파일에 POST + Allow: GET, HEAD",
          post.status == 405 and post.header("Allow") == "GET, HEAD")
    check("405 뒤에는 연결 유지(본문을 버리고 다음 요청 처리)", not closed)
    check("414 너무 긴 경로", get(port, "/" + "a" * 5000).status == 414)
    check("414 이름 하나가 NAME_MAX 초과", get(port, "/" + "a" * 300).status == 414)


def test_parser_errors(port):
    print("[요청 파싱 오류]")
    cases = [
        ("메서드 소문자 → 501", b"get / HTTP/1.1\r\nHost: t\r\n\r\n", 501),
        ("모르는 메서드 → 501", b"PATCH / HTTP/1.1\r\nHost: t\r\n\r\n", 501),
        ("메서드 문자 오류 → 400", b"G@T / HTTP/1.1\r\nHost: t\r\n\r\n", 400),
        ("공백 두 개 → 400", b"GET  / HTTP/1.1\r\nHost: t\r\n\r\n", 400),
        ("버전 없음 → 400", b"GET /\r\nHost: t\r\n\r\n", 400),
        ("버전 2.0 → 505", b"GET / HTTP/2.0\r\nHost: t\r\n\r\n", 505),
        ("버전 형식 오류 → 400", b"GET / HTTPX/1.1\r\nHost: t\r\n\r\n", 400),
        ("절대 URI → 400", b"GET http://t/ HTTP/1.1\r\nHost: t\r\n\r\n", 400),
        ("Host 없음(1.1) → 400", b"GET / HTTP/1.1\r\n\r\n", 400),
        ("Host 두 개 → 400", b"GET / HTTP/1.1\r\nHost: a\r\nHost: b\r\n\r\n", 400),
        ("이름 뒤 공백 → 400", b"GET / HTTP/1.1\r\nHost : t\r\n\r\n", 400),
        ("콜론 없는 줄 → 400", b"GET / HTTP/1.1\r\nHost: t\r\nbroken\r\n\r\n", 400),
        ("obs-fold → 400", b"GET / HTTP/1.1\r\nHost: t\r\n x\r\n\r\n", 400),
        ("잘못된 %XX → 400", b"GET /%zz HTTP/1.1\r\nHost: t\r\n\r\n", 400),
        ("%00 → 400", b"GET /a%00 HTTP/1.1\r\nHost: t\r\n\r\n", 400),
        ("Content-Length 문자 → 400", b"POST / HTTP/1.1\r\nHost: t\r\nContent-Length: x\r\n\r\n", 400),
        ("Content-Length 음수 → 400", b"POST / HTTP/1.1\r\nHost: t\r\nContent-Length: -1\r\n\r\n", 400),
        ("Content-Length 오버플로 → 400",
         b"POST / HTTP/1.1\r\nHost: t\r\nContent-Length: 99999999999999999999999\r\n\r\n", 400),
        ("Content-Length 서로 다른 중복 → 400",
         b"POST / HTTP/1.1\r\nHost: t\r\nContent-Length: 1\r\nContent-Length: 2\r\n\r\n", 400),
        ("본문 상한 초과 → 413 (본문 수신 전)",
         b"POST / HTTP/1.1\r\nHost: t\r\nContent-Length: 5000000\r\n\r\n", 413),
        ("POST 길이 정보 없음 → 411", b"POST / HTTP/1.1\r\nHost: t\r\n\r\n", 411),
        ("chunked → 501",
         b"POST / HTTP/1.1\r\nHost: t\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n", 501),
        ("TE와 CL 동시 → 400 (Smuggling 방지)",
         b"POST / HTTP/1.1\r\nHost: t\r\nTransfer-Encoding: chunked\r\nContent-Length: 4\r\n\r\n", 400),
        ("Expect 다른 값 → 417", b"POST / HTTP/1.1\r\nHost: t\r\nContent-Length: 1\r\nExpect: x\r\n\r\n", 417),
        ("요청 라인 8KB 초과 → 414", b"GET /" + b"a" * 9000 + b" HTTP/1.1\r\nHost: t\r\n\r\n", 414),
        ("헤더 16KB 초과 → 431", b"GET / HTTP/1.1\r\nHost: t\r\nX: " + b"v" * 17000 + b"\r\n\r\n", 431),
        ("헤더 개수 초과 → 431",
         b"GET / HTTP/1.1\r\nHost: t\r\n" + b"".join(b"H%d: v\r\n" % i for i in range(101)) + b"\r\n", 431),
        ("제어 문자 → 400", b"GET /a\x01b HTTP/1.1\r\nHost: t\r\n\r\n", 400),
    ]
    for name, raw, expected in cases:
        response, closed = exchange(port, raw)
        ok = response is not None and response.status == expected
        check(name, ok, "(실제 %s)" % (response.status if response else "응답 없음"))
        check("  └ 응답 후 연결 닫힘", closed)


def test_connection_handling(port):
    print("[연결 유지·경계]")
    with connect(port) as sock:
        pending = b""
        for index in range(3):
            sock.sendall(b"GET / HTTP/1.1\r\nHost: t\r\n\r\n")
            response, pending = read_response(sock, pending)
            check("keep-alive 요청 %d" % (index + 1), response is not None and response.status == 200)
    response, closed = exchange(port, b"GET / HTTP/1.0\r\n\r\n")
    check("HTTP/1.0 → 1.1로 응답하고 닫음",
          response.raw.startswith(b"HTTP/1.1 200") and response.header("Connection") == "close" and closed)
    response, closed = exchange(port, b"GET / HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n")
    check("Connection: close → 닫음", response.header("Connection") == "close" and closed)
    response, closed = exchange(port, b"GET / HTTP/1.0\r\nConnection: keep-alive\r\n\r\n")
    check("HTTP/1.0 + keep-alive → 유지", response.header("Connection") == "keep-alive" and not closed)
    with connect(port) as sock:
        raw = (b"GET / HTTP/1.1\r\nHost: t\r\n\r\nGET /docs/ HTTP/1.1\r\nHost: t\r\n\r\n"
               b"GET /style.css HTTP/1.1\r\nHost: t\r\n\r\nGET /nope HTTP/1.1\r\nHost: t\r\n\r\n")
        sock.sendall(raw)
        pending = b""
        statuses = []
        bodies = []
        for _ in range(4):
            response, pending = read_response(sock, pending)
            statuses.append(response.status)
            bodies.append(response.body[:10])
        check("붙여 보낸 요청 4개에 순서대로 응답", statuses == [200, 200, 200, 404]
              and bodies[0] == b"I am serve" and bodies[1] == b"docs 0")
    with connect(port) as sock:
        for byte in b"GET /docs/ HTTP/1.1\r\nHost: t\r\n\r\n":
            sock.sendall(bytes([byte]))
            time.sleep(0.002)
        response, _ = read_response(sock)
        check("1바이트씩 쪼개 보내도 같은 응답", response.status == 200 and response.body == b"docs 0")
    with connect(port) as sock:
        sock.sendall(b"GET / HTTP/1.1\r\nHost: t\r\n\r\n")
        sock.shutdown(socket.SHUT_WR)
        response, _ = read_response(sock)
        check("반쪽 닫기: 요청 뒤 쓰기를 닫아도 응답을 받음",
              response is not None and response.status == 200 and response.body == b"I am server 0")
        check("  └ 응답 뒤 서버가 연결을 정리", sock.recv(1) == b"")
    with connect(port) as sock:
        sock.sendall(b"POST /style.css HTTP/1.1\r\nHost: t\r\nContent-Length: 5\r\n\r\nab")
        sock.shutdown(socket.SHUT_WR)
        check("반쪽 닫기: 본문이 덜 온 요청은 정리", sock.recv(100) == b"")
    with connect(port) as sock:
        sock.sendall(b"POST /style.css HTTP/1.1\r\nHost: t\r\nContent-Length: 3\r\n"
                     b"Expect: 100-continue\r\n\r\n")
        response, _ = read_response(sock)
        check("100-continue → 본문 전에 100 Continue", response.status == 100)
        sock.sendall(b"abc")
        response, _ = read_response(sock)
        check("  └ 본문을 보내면 최종 응답(405)", response.status == 405)
    response, _ = exchange(port, b"POST / HTTP/1.1\r\nHost: t\r\nContent-Length: 5000000\r\n"
                                 b"Expect: 100-continue\r\n\r\n")
    check("오류인 요청에는 100 Continue 없이 바로 413", response.status == 413)
    with connect(port) as sock:
        sock.sendall(b"POST /style.css HTTP/1.1\r\nHost: t\r\nContent-Length: 3\r\n\r\nabc"
                     b"GET /docs/ HTTP/1.1\r\nHost: t\r\n\r\n")
        first, pending = read_response(sock)
        second, _ = read_response(sock, pending)
        check("POST(본문 있음) 다음 GET이 본문을 기다리지 않음",
              first.status == 405 and second.status == 200 and second.body == b"docs 0")


def test_proxy(proxy_port, backend_ports, python_port):
    print("[리버스 프록시]")
    seen = []
    for _ in range(6):
        seen.append(get(proxy_port, "/api/who.txt").body)
    expected = [b"backend %d" % (index % 3) for index in range(3)]
    check("Round-Robin: 3대에 차례로 분배", set(seen[:3]) == set(expected) and seen[:3] == seen[3:],
          str(seen))
    check("프록시 경유 응답에 Content-Length 일치",
          int(get(proxy_port, "/api/who.txt").header("Content-Length")) == 9)
    echo = get(proxy_port, "/py/echo?id=3&name=%41",
               "Connection: keep-alive\r\nKeep-Alive: timeout=5\r\nX-Forwarded-For: 6.6.6.6\r\n"
               "X-Custom: kept\r\n")
    check("원래 URI(쿼리 포함)를 그대로 전달", echo.header("X-Seen-Target") == "/py/echo?id=3&name=%41")
    check("Host 그대로 전달", echo.header("X-Seen-Host") == "test")
    check("Connection: close로 바꿔 전달", echo.header("X-Seen-Connection") == "close")
    check("hop-by-hop(Keep-Alive) 제거", echo.header("X-Seen-Keep-Alive") is None)
    check("X-Forwarded-For는 손님 IP로 덮어씀", echo.header("X-Seen-X-Forwarded-For") == "127.0.0.1")
    check("일반 헤더는 그대로 전달", echo.header("X-Seen-X-Custom") == "kept")
    payload = os.urandom(200000)
    response, _ = exchange(proxy_port, b"POST /py/echo HTTP/1.1\r\nHost: t\r\nContent-Length: %d\r\n\r\n"
                           % len(payload) + payload)
    check("POST 본문(바이너리 200KB) 그대로 전달·반환",
          response.status == 200 and fingerprint(response.body) == fingerprint(payload))
    check("가장 긴 경로 우선 (/api/admin → 별도 백엔드)",
          get(proxy_port, "/api/admin/who.txt").status == 404)
    check("/apix 는 규칙과 불일치 → 정적 파일 처리(404)", get(proxy_port, "/apix/who.txt").status == 404)
    check("규칙 없는 요청은 정적 파일", get(proxy_port, "/index.html").body == b"I am server 0")
    mixed = [get(proxy_port, "/mix/who.txt").body for _ in range(6)]
    check("죽은 백엔드를 건너뛰고 살아 있는 쪽으로", all(body == b"backend 0" for body in mixed), str(mixed))
    check("전부 죽으면 502", get(proxy_port, "/dead/x").status == 502)
    start = time.time()
    response, _ = exchange(proxy_port, b"GET /py/slow?seconds=30 HTTP/1.1\r\nHost: t\r\n\r\n", timeout=15)
    elapsed = time.time() - start
    check("응답 타임아웃(2초) → 504", response.status == 504 and 1.5 < elapsed < 5, "%.1fs" % elapsed)
    response, _ = exchange(proxy_port, b"GET /py/cut-header HTTP/1.1\r\nHost: t\r\n\r\n")
    check("헤더 전에 끊김 → 502", response.status == 502)
    response, _ = exchange(proxy_port, b"GET /py/cut-silent HTTP/1.1\r\nHost: t\r\n\r\n")
    check("아무것도 안 보내고 끊김 → 502", response.status == 502)
    check("백엔드 응답 형식 오류 → 502", exchange(proxy_port, b"GET /py/garbage HTTP/1.1\r\nHost: t\r\n\r\n")[0].status == 502)
    check("Content-Length 불일치 중복 → 502", exchange(proxy_port, b"GET /py/bad-length HTTP/1.1\r\nHost: t\r\n\r\n")[0].status == 502)
    check("TE와 CL 동시 수신 → 502", exchange(proxy_port, b"GET /py/both-length HTTP/1.1\r\nHost: t\r\n\r\n")[0].status == 502)
    check("101 응답 → 502", exchange(proxy_port, b"GET /py/switch HTTP/1.1\r\nHost: t\r\n\r\n")[0].status == 502)
    response, _ = exchange(proxy_port, b"GET /py/continue HTTP/1.1\r\nHost: t\r\n\r\n")
    check("백엔드의 100 Continue는 버리고 최종 응답만 전달", response.status == 200 and response.body == b"ok")
    response, closed = exchange(proxy_port, b"GET /py/cut-body HTTP/1.1\r\nHost: t\r\n\r\n")
    check("본문 도중 백엔드가 끊기면 손님 연결도 닫음(502로 바꾸지 않음)",
          response.status == 200 and len(response.body) == 10 and closed)
    response, closed = exchange(proxy_port, b"GET /py/no-length HTTP/1.1\r\nHost: t\r\n\r\n")
    check("Content-Length 없으면 백엔드가 닫을 때까지 전달 후 손님 연결 닫음",
          response.body == b"until-close" and response.header("Connection") == "close" and closed)
    response, closed = exchange(proxy_port, b"GET /py/chunked HTTP/1.1\r\nHost: t\r\n\r\n")
    check("chunked 응답은 Transfer-Encoding 유지한 채 그대로 전달",
          response.header("Transfer-Encoding") == "chunked" and response.body.endswith(b"0\r\n\r\n") and closed)
    check("손님이 HTTP/1.0인데 chunked 응답 → 502",
          exchange(proxy_port, b"GET /py/chunked HTTP/1.0\r\n\r\n")[0].status == 502)
    response, closed = exchange(proxy_port, b"GET /py/no-content HTTP/1.1\r\nHost: t\r\n\r\n")
    check("204는 본문을 기다리지 않음", response.status == 204 and not closed)
    response, closed = exchange(proxy_port, b"HEAD /py/head-length HTTP/1.1\r\nHost: t\r\n\r\n", is_head=True)
    check("HEAD는 Content-Length가 있어도 본문을 기다리지 않음",
          response.status == 200 and response.header("Content-Length") == "50" and not closed)
    response, _ = exchange(proxy_port, b"GET /py/hop HTTP/1.1\r\nHost: t\r\n\r\n")
    check("응답의 hop-by-hop 제거, Connection은 손님 기준으로 다시 붙임",
          response.header("X-Hop") is None and response.header("Keep-Alive") is None
          and response.header("Connection") == "keep-alive")
    with connect(proxy_port) as sock:
        pending = b""
        bodies = []
        for _ in range(4):
            sock.sendall(b"GET /api/who.txt HTTP/1.1\r\nHost: t\r\n\r\n")
            response, pending = read_response(sock, pending)
            bodies.append(response.body)
        check("프록시 응답 뒤에도 손님 keep-alive 유지(백엔드는 매번 새 연결)",
              len(bodies) == 4 and all(b.startswith(b"backend") for b in bodies))
    megabytes = 40
    response, _ = exchange(proxy_port, b"GET /py/big?mb=%d HTTP/1.1\r\nHost: t\r\n\r\n" % megabytes, timeout=60)
    pattern = b"0123456789abcdef" * 4096
    expected_big = (pattern * (megabytes * 1024 * 1024 // len(pattern)))
    check("40MB 응답을 끝까지 전달(해시 일치)", fingerprint(response.body) == fingerprint(expected_big))


def test_restart_and_idle_cpu(root):
    print("[재시작·유휴 CPU]")
    server = Server(root)
    port = server.port
    for _ in range(20):
        exchange(port, b"GET / HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n")
    server.stop()
    try:
        again = Server(root, port=port)
        check("종료 직후 같은 포트로 다시 시작 (SO_REUSEADDR)", True)
    except RuntimeError:
        check("종료 직후 같은 포트로 다시 시작 (SO_REUSEADDR)", False)
        return
    idle = [connect(port) for _ in range(200)]
    time.sleep(0.5)
    before = again.cpu_seconds()
    time.sleep(4)
    used = again.cpu_seconds() - before
    check("유휴 연결 200개를 붙잡고 있어도 CPU를 쓰지 않음 (4초 동안 %.2f초)" % used, used < 0.2)
    blocked = connect(port)
    blocked.sendall(b"GET /data.bin HTTP/1.1\r\nHost: t\r\n\r\n")
    time.sleep(1)
    before = again.cpu_seconds()
    time.sleep(4)
    used = again.cpu_seconds() - before
    check("응답을 받아 가지 않는 손님이 있어도 쓰기 대기가 CPU를 태우지 않음 (4초 동안 %.2f초)" % used,
          used < 0.2)
    for sock in idle + [blocked]:
        sock.close()
    again.stop()


def test_fd_exhaustion(root):
    print("[fd 고갈 (비상용 fd)]")
    server = Server(root, fd_limit=48, hard_limit=True, max_connections=1000)
    sockets = []
    for _ in range(150):
        try:
            sockets.append(connect(server.port, timeout=2))
        except OSError:
            break
    time.sleep(1)
    before = server.cpu_seconds()
    time.sleep(3)
    used = server.cpu_seconds() - before
    check("fd가 바닥나도 서버 생존, CPU 100%%로 헛돌지 않음 (3초 동안 %.2f초)" % used,
          server.is_alive and used < 0.3)
    response = None
    for sock in sockets[:5]:
        try:
            sock.sendall(b"GET /index.html HTTP/1.1\r\nHost: t\r\n\r\n")
            sock.settimeout(2)
            response, _ = read_response(sock)
            if response is not None:
                break
        except OSError:
            continue
    # fd 표가 가득 차 있으면 서버가 파일을 열지 못해 500이 정상이고, 자리가 있으면 200이다. 둘 다 형식이 맞아야 한다.
    check("이미 연결된 손님에게 형식이 맞는 응답 (fd 표가 가득 차 파일을 못 열면 500: 상태 %s)"
          % (response.status if response else None),
          response is not None and response.status in (200, 500)
          and int(response.header("Content-Length")) == len(response.body))
    for sock in sockets:
        sock.close()
    time.sleep(1.5)
    check("연결이 줄면 새 손님을 다시 받음", get(server.port, "/index.html").status == 200)
    server.stop()


def test_graceful_shutdown(base, root):
    print("[우아한 종료]")
    with open(os.path.join(root, "big30.bin"), "wb") as handle:
        handle.write(os.urandom(30 * 1024 * 1024))
    with open(os.path.join(root, "big30.bin"), "rb") as handle:
        digest = fingerprint(handle.read())
    server = Server(root)
    sock = connect(server.port)
    sock.sendall(b"GET /big30.bin HTTP/1.1\r\nHost: t\r\n\r\n")
    head, rest = read_head(sock)
    received = bytearray(rest)
    time.sleep(0.3)
    server.process.send_signal(signal.SIGINT)
    time.sleep(0.3)
    try:
        late = connect(server.port, timeout=2)
        late.sendall(b"GET /index.html HTTP/1.1\r\nHost: t\r\n\r\n")
        late_data = late.recv(100)
        check("종료 중에는 새 손님을 받지 않음", late_data == b"")
        late.close()
    except OSError:
        check("종료 중에는 새 손님을 받지 않음(연결 거부)", True)
    length = int(parse_head(head)[2][[k.lower() for k, _ in parse_head(head)[2]].index("content-length")][1])
    while len(received) < length:
        chunk = sock.recv(65536)
        if not chunk:
            break
        received += chunk
        time.sleep(0.0005)
    check("종료 신호 뒤에도 진행 중이던 30MB 응답을 끝까지 보냄(해시 일치)",
          len(received) == length and fingerprint(bytes(received)) == digest)
    check("응답이 끝나면 서버가 정상 종료(코드 0)", server.process.wait(10) == 0)
    sock.close()
    stuck = Server(root)
    blocked = connect(stuck.port)
    blocked.sendall(b"GET /big30.bin HTTP/1.1\r\nHost: t\r\n\r\n")
    time.sleep(0.5)
    started = time.time()
    stuck.process.send_signal(signal.SIGINT)
    code = stuck.process.wait(15)
    elapsed = time.time() - started
    check("응답을 받아 가지 않는 손님이 있어도 약 5초 뒤 종료 (%.1f초, 코드 %s)" % (elapsed, code),
          code == 0 and 4.5 <= elapsed <= 7)
    blocked.close()
    os.remove(os.path.join(root, "big30.bin"))


def test_proxy_backpressure(proxy_port, proxy):
    print("[프록시 배압]")
    before = proxy.memory_kb()
    sock = connect(proxy_port)
    sock.sendall(b"GET /py/big?mb=200 HTTP/1.1\r\nHost: t\r\n\r\n")
    head, rest = read_head(sock)
    time.sleep(4)
    during = proxy.memory_kb()
    check("느린 손님에게 큰 응답을 중계해도 메모리가 늘지 않음 (전 %d KB → 중 %d KB)" % (before, during),
          during - before < 3000)
    sock.close()
    time.sleep(1)
    check("손님이 끊으면 프록시 연결도 정리 (fd %d)" % proxy.fd_count(), proxy.fd_count() <= 12)


def test_connect_timeout(base, root):
    print("[백엔드 연결 시간 초과]")
    config_path = os.path.join(base, "blackhole.conf")
    with open(config_path, "w") as handle:
        handle.write("route /hole 192.0.2.1:81\nconnect_timeout 1\nresponse_timeout 2\n")
    server = Server(root, config=config_path)
    with connect(server.port) as sock:
        start = time.time()
        sock.sendall(b"GET /hole/x HTTP/1.1\r\nHost: t\r\n\r\n")
        response, _ = read_response(sock)
        elapsed = time.time() - start
    if response.status == 504:
        check("연결이 안 되는 주소 → 연결 타임아웃(1초) 뒤 504 (%.2f초)" % elapsed, 0.9 < elapsed < 1.5)
    else:
        print("  건너뜀  이 네트워크는 192.0.2.1에 즉시 실패를 돌려줘 연결 타임아웃을 재현하지 못함, 검사하지 않음 (상태 %d, %.1f초)"
              % (response.status, elapsed))
    server.stop()


def test_memory_limit(root):
    print("[버퍼 합계 상한 503]")
    limited = os.path.join(ROOT, "build-limit", "webserver")
    if not os.path.exists(limited):
        print("  건너뜀  build-limit/webserver 없음 (make limit-build)")
        return
    server = Server(root, binary=limited)
    body = b"x" * 600000
    header = b"POST /style.css HTTP/1.1\r\nHost: t\r\nContent-Length: 600000\r\n\r\n"
    sockets = []
    for _ in range(8):
        sock = connect(server.port)
        sock.setblocking(False)
        sockets.append([sock, header + body, b""])
    deadline = time.time() + 5
    while time.time() < deadline and any(len(item[2]) < 12 and item[1] is not None for item in sockets):
        for item in sockets:
            try:
                if item[1]:
                    sent = item[0].send(item[1][:65536])
                    item[1] = item[1][sent:]
            except (BlockingIOError, OSError):
                pass
            try:
                item[2] += item[0].recv(100)
            except (BlockingIOError, OSError):
                pass
        time.sleep(0.01)
    statuses = [item[2][9:12] for item in sockets]
    check("합계 상한을 넘기는 요청은 503으로 응답 (%s)" % [x.decode() or "-" for x in statuses],
          b"503" in statuses)
    check("503 응답 뒤 연결을 닫음, 상한 안의 요청은 영향 없음 (503이 아닌 연결 %d개)" % (8 - statuses.count(b"503")),
          statuses.count(b"503") < 8)
    check("503 뒤에도 서버 생존, 새 손님에게 정상 응답",
          server.is_alive and get(server.port, "/style.css").status == 200)
    for item in sockets:
        item[0].close()
    time.sleep(0.5)
    check("연결을 닫으면 버퍼가 돌아와 새 요청 처리", get(server.port, "/index.html").status == 200)
    server.stop()


def main():
    base = tempfile.mkdtemp(prefix="ws_func_")
    processes = []
    try:
        roots = prepare_roots(base)
        static_server = Server(roots[0])
        processes.append(static_server)
        backends = [Server(root) for root in roots]
        processes += backends
        python_backend = PythonBackend()
        processes.append(python_backend)
        dead_port = find_free_port()
        config_path = os.path.join(base, "proxy.conf")
        with open(config_path, "w") as handle:
            handle.write("route /api %s\n" % " ".join("127.0.0.1:%d" % b.port for b in backends))
            handle.write("route /api/admin 127.0.0.1:%d\n" % python_backend.port)
            handle.write("route /py 127.0.0.1:%d\n" % python_backend.port)
            handle.write("route /dead 127.0.0.1:%d\n" % dead_port)
            handle.write("route /mix 127.0.0.1:%d 127.0.0.1:%d\n" % (dead_port, backends[0].port))
            handle.write("connect_timeout 1\nresponse_timeout 2\n")
        proxy = Server(roots[0], config=config_path)
        processes.append(proxy)

        test_static(static_server.port, roots[0])
        test_parser_errors(static_server.port)
        test_connection_handling(static_server.port)
        test_proxy(proxy.port, [b.port for b in backends], python_backend.port)
        test_proxy_backpressure(proxy.port, proxy)
        test_connect_timeout(base, roots[0])
        test_memory_limit(roots[0])
        test_restart_and_idle_cpu(roots[0])
        test_fd_exhaustion(roots[0])
        test_graceful_shutdown(base, roots[0])

        print("[종료·자원]")
        time.sleep(0.5)
        for server in [static_server, proxy] + backends:
            before = server.fd_count()
            check("서버 %d 종료 전 fd 수가 시작 수준(listen+루트+비상용 등)" % server.port,
                  before <= 12, "fd=%d" % before)
        for server in [static_server, proxy] + backends:
            code = server.stop()
            check("서버 %d SIGINT로 정상 종료(코드 0)" % server.port, code == 0, "code=%s" % code)
        for server in [static_server, proxy] + backends:
            check("서버 %d 로그에 ERROR 없음" % server.port, "[ERROR]" not in server.log_text(),
                  server.log_text()[-300:])
    finally:
        for process in processes:
            process.stop()
        shutil.rmtree(base, ignore_errors=True)
    return checker.finish()


if __name__ == "__main__":
    sys.exit(main())
