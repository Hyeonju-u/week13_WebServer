"""데모 전용 백엔드: 자기 포트 번호와 받은 X-Forwarded-For를 본문으로 돌려준다.
어느 백엔드가 응답했는지 보여 주려고 쓴다 (Round-Robin, 장애 제외, X-Forwarded-For 덮어쓰기).
사용법: python3 demo/name_backend.py <포트>"""
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        body = ("backend %d | X-Forwarded-For: %s\n" % (
            self.server.server_address[1], self.headers.get("X-Forwarded-For", "-"))).encode()
        self.send_response(200)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *arguments):
        pass


def main():
    ThreadingHTTPServer.daemon_threads = True
    ThreadingHTTPServer(("127.0.0.1", int(sys.argv[1])), Handler).serve_forever()


if __name__ == "__main__":
    main()
