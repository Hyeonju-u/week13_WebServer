"""4단계 실험용 백엔드 (G16)

사용: python3 backend/backend.py <포트>

127.0.0.1:<포트>에서 듣는다. 응답은 항상 Content-Length를 붙이고
HTTP/1.0(요청마다 닫음)으로 답한다.

  GET  /api/hello        {"port": 포트}
  POST /api/echo         받은 본문 그대로
  GET  /api/slow?ms=N    N밀리초 기다린 뒤 {"port": 포트, "slept_ms": N}
  GET  /api/headers      {"request_line": "...", "headers": [[이름, 값], ...]}
  그 밖                   404
"""

import json
import sys
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlsplit


class BackendHandler(BaseHTTPRequestHandler):
    def do_GET(self):
        path = urlsplit(self.path).path
        if path == "/api/hello":
            self.send_json({"port": self.server.server_port})
            return
        if path == "/api/slow":
            self.handle_slow()
            return
        if path == "/api/headers":
            self.send_json({"request_line": self.requestline,
                            "headers": [[name, value] for name, value
                                        in self.headers.items()]})
            return
        self.send_body(404, "text/plain", b"not found\n")

    def do_POST(self):
        if urlsplit(self.path).path != "/api/echo":
            self.send_body(404, "text/plain", b"not found\n")
            return
        length = int(self.headers.get("Content-Length", "0"))
        body = self.rfile.read(length)
        self.send_body(200, "application/octet-stream", body)

    def handle_slow(self):
        query = parse_qs(urlsplit(self.path).query)
        milliseconds = int(query.get("ms", ["0"])[0])
        time.sleep(milliseconds / 1000)
        self.send_json({"port": self.server.server_port,
                        "slept_ms": milliseconds})

    def send_json(self, value):
        self.send_body(200, "application/json",
                       json.dumps(value).encode("utf-8"))

    def send_body(self, status, content_type, body):
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, format, *arguments):
        # 요청마다 stderr에 찍으면 테스트 출력이 묻힌다
        pass


class BackendServer(ThreadingHTTPServer):
    # 기본 listen 대기열 5로는 손님이 몰리면(CL-2) SYN이 버려져
    # 프록시 쪽 connect 타임아웃(504)이 난다
    request_queue_size = 128
    daemon_threads = True


def main():
    if len(sys.argv) != 2:
        print("usage: %s <port>" % sys.argv[0], file=sys.stderr)
        return 2
    server = BackendServer(("127.0.0.1", int(sys.argv[1])), BackendHandler)
    server.serve_forever()
    return 0


if __name__ == "__main__":
    sys.exit(main())
