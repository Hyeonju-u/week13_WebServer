import concurrent.futures
import contextlib
import http.client
import json
import os
import socket
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

SERVER = Path(sys.argv.pop(1)).resolve()


class ServerChecks(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        root = Path(cls.temp.name) / "public"
        root.mkdir()
        (root / "index.html").write_text("안녕", encoding="utf-8")
        cls.binary = bytes(range(256)) * 8193
        (root / "sample.bin").write_bytes(cls.binary)
        (root / "alias").symlink_to(Path(cls.temp.name))
        cls.server = subprocess.Popen(
            [SERVER, "0", root, "127.0.0.1"], stdout=subprocess.PIPE,
            text=True, env=os.environ.copy()
        )
        cls.addClassCleanup(cls.stop_server)
        cls.port = int(cls.server.stdout.readline().strip().rsplit(":", 1)[1])

    @classmethod
    def stop_server(cls):
        cls.server.terminate()
        cls.server.wait(timeout=5)
        cls.server.stdout.close()

    def connect(self):
        return socket.create_connection(("127.0.0.1", self.port), timeout=5)

    def exchange(self, request):
        with self.connect() as client:
            client.sendall(request)
            return self.read_response(client)

    def read_response(self, client):
        with http.client.HTTPResponse(client) as response:
            response.begin()
            body = response.read()
            self.assertEqual(len(body), int(response.getheader("Content-Length")))
            self.assertEqual(response.getheader("Connection"), "close")
            self.assertEqual(client.recv(1), b"")
            return response.status, response.getheader("Content-Type"), body

    def get(self, target):
        return self.exchange(b"GET " + target + b" HTTP/1.1\r\nHost: x\r\n\r\n")

    def test_static_and_mime(self):
        self.assertEqual(self.get(b"/?v=1"),
                         (200, "text/html; charset=utf-8", "안녕".encode()))
        self.assertEqual(self.get(b"/sample.bin"),
                         (200, "application/octet-stream", self.binary))
        self.assertEqual(self.get(b"/absent")[0], 404)
        self.assertIn(self.get(b"/alias/index.html")[0], (403, 404))

    def test_fragmented_header_and_body(self):
        body = json.dumps({"message": "안녕"}, ensure_ascii=False).encode()
        with self.connect() as slow:
            slow.sendall(b"POST /api/echo HTTP/1.1\r\nHo")
            self.assertEqual(self.get(b"/")[0], 200)
            slow.sendall(b"st: x\r\nContent-Length: %d\r\n\r" % len(body))
            self.assertEqual(self.get(b"/")[0], 200)
            slow.sendall(b"\n" + body[:3])
            self.assertEqual(self.get(b"/")[0], 200)
            slow.sendall(body[3:])
            self.assertEqual(self.read_response(slow), (200, "application/json", body))

    def test_slow_reader(self):
        with self.connect() as slow:
            slow.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
            slow.sendall(b"GET /sample.bin HTTP/1.1\r\nHost: x\r\n\r\n")
            self.assertEqual(self.get(b"/")[0], 200)

    def test_parallel(self):
        with concurrent.futures.ThreadPoolExecutor(max_workers=16) as pool:
            for status, _, _ in pool.map(self.get, [b"/"] * 80):
                self.assertEqual(status, 200)

    def test_listener_resumes_after_capacity(self):
        with contextlib.ExitStack() as stack:
            held = [stack.enter_context(self.connect()) for _ in range(128)]
            for client in held:
                client.sendall(b"GET / HTTP/1.1\r\n")
            queued = stack.enter_context(self.connect())
            queued.sendall(b"GET / HTTP/1.1\r\nHost: x\r\n\r\n")
            held[0].sendall(b"Host: x\r\n\r\n")
            self.assertEqual(self.read_response(held[0])[0], 200)
            self.assertEqual(self.read_response(queued)[0], 200)

    def test_complete_request_with_half_close(self):
        with self.connect() as client:
            client.sendall(b"GET / HTTP/1.1\r\nHost: x\r\n\r\n")
            client.shutdown(socket.SHUT_WR)
            self.assertEqual(self.read_response(client)[0], 200)

    def test_limits_and_errors(self):
        self.assertEqual(self.get(b"/" + b"a" * 4096)[0], 414)
        self.assertEqual(self.get(b"/" + b"a" * 18000)[0], 414)
        self.assertEqual(self.exchange(b"GET / HTTP/1.1\r\nX: " + b"x" * 16366)[0], 431)
        self.assertEqual(self.exchange(b"POST /api/echo HTTP/1.1\r\nHost: x\r\n"
                                       b"Content-Length: 1048577\r\n\r\n")[0], 413)
        self.assertEqual(self.exchange(b"PUT / HTTP/1.1\r\nHost: x\r\n\r\n")[0], 405)
        self.assertEqual(self.exchange(b"GET / HTTP/1.0\r\nHost: x\r\n\r\n")[0], 400)
        with self.connect() as client:
            client.sendall(b"GET / HTTP/1.1\r\n")
            client.shutdown(socket.SHUT_WR)
            self.assertEqual(self.read_response(client)[0], 400)

    def test_disconnect(self):
        with self.connect() as client:
            client.sendall(b"POST /api/echo HTTP/1.1\r\nHost: x\r\nContent-Length: 7\r\n\r\nx")
        self.assertEqual(self.get(b"/")[0], 200)


if __name__ == "__main__":
    unittest.main()
