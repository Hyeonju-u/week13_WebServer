import contextlib
import http.client
import socket
import subprocess
import sys
import threading
import unittest
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

SERVER = Path(sys.argv.pop(1)).resolve()


class Origin(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_GET(self):
        if self.path == "/slow":
            self.server.waiting.set()
            self.server.release.wait(8)
            return
        if self.path == "/truncated":
            self.connection.sendall(b"HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\nx")
            return
        if self.path == "/empty":
            self.send_response(204)
            self.end_headers()
            return
        if self.path == "/stream":
            self.send_response(200)
            self.end_headers()
            self.wfile.write(b"close-delimited")
            return
        self.reply(404, self.path.encode(), "text/plain")

    def do_POST(self):
        body = self.rfile.read(int(self.headers["Content-Length"]))
        self.server.last_request = self.path, self.headers["Host"]
        self.reply(201, body, self.headers["Content-Type"])

    def reply(self, status, body, content_type):
        self.send_response(status)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Content-Type", content_type)
        self.end_headers()
        self.wfile.write(body)


class ProxyChecks(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.origin = ThreadingHTTPServer(("127.0.0.1", 0), Origin)
        cls.origin.waiting = threading.Event()
        cls.origin.release = threading.Event()
        cls.addClassCleanup(cls.origin.server_close)
        threading.Thread(target=cls.origin.serve_forever, daemon=True).start()
        cls.addClassCleanup(cls.origin.shutdown)
        cls.addClassCleanup(cls.origin.release.set)
        cls.port = cls.start_server(cls.origin.server_port)

    @classmethod
    def start_server(cls, backend_port):
        process = subprocess.Popen(
            [SERVER, "0", "public", "127.0.0.1", "/edge", "127.0.0.1",
             str(backend_port)], stdout=subprocess.PIPE, text=True
        )
        cls.addClassCleanup(process.stdout.close)
        cls.addClassCleanup(process.wait, 5)
        cls.addClassCleanup(process.terminate)
        return int(process.stdout.readline().strip().rsplit(":", 1)[1])

    def request(self, path, method="GET", body=None, headers=None, port=None):
        with contextlib.closing(http.client.HTTPConnection(
                "127.0.0.1", port or self.port, timeout=6)) as conn:
            conn.request(method, path, body=body, headers=headers or {})
            response = conn.getresponse()
            return response.status, response.getheader("Content-Type"), response.read()

    def test_post_and_query(self):
        body = bytes(range(256)) * 2048
        self.assertEqual(self.request("/edge/api?q=hi%20there", "POST", body,
                                      {"Content-Type": "application/octet-stream"}),
                         (201, "application/octet-stream", body))
        self.assertEqual(self.origin.last_request,
                         ("/api?q=hi%20there", f"127.0.0.1:{self.origin.server_port}"))

    def test_route_and_status(self):
        self.assertEqual(self.request("/edge"), (404, "text/plain", b"/"))
        self.assertEqual(self.request("/edge/missing"), (404, "text/plain", b"/missing"))
        self.assertEqual(self.request("/edgecase")[2], b"404 Not Found")
        self.assertEqual(self.request("/edge/empty")[::2], (204, b""))
        self.assertEqual(self.request("/edge/stream")[::2], (200, b"close-delimited"))

    def test_incomplete_reply(self):
        self.assertEqual(self.request("/edge/truncated")[0], 502)

    def test_refused_connection(self):
        with socket.socket() as reserved:
            reserved.bind(("127.0.0.1", 0))
            port = self.start_server(reserved.getsockname()[1])
            self.assertEqual(self.request("/edge/", port=port)[0], 502)

    def test_timeout_and_concurrency(self):
        with contextlib.closing(http.client.HTTPConnection(
                "127.0.0.1", self.port, timeout=6)) as slow:
            slow.request("GET", "/edge/slow")
            self.assertTrue(self.origin.waiting.wait(1))
            with contextlib.closing(http.client.HTTPConnection(
                    "127.0.0.1", self.port, timeout=1)) as fast:
                fast.request("GET", "/sample.html")
                response = fast.getresponse()
                self.assertEqual(response.status, 200)
                self.assertIn(b"<h1>", response.read())
            self.assertEqual(slow.getresponse().status, 504)
        self.origin.release.set()


if __name__ == "__main__":
    unittest.main()
