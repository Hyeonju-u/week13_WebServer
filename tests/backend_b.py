#!/usr/bin/env python3
"""테스트 전용 백엔드 B (10-10): 응답 바이트를 직접 조작하려고 socket만 쓴다."""
import socket
import sys
import threading
import time

PATTERN = b"0123456789abcdef" * 4096


def read_request(connection):
    data = b""
    while b"\r\n\r\n" not in data:
        chunk = connection.recv(65536)
        if not chunk:
            return None
        data += chunk
    head, _, rest = data.partition(b"\r\n\r\n")
    lines = head.split(b"\r\n")
    headers = {}
    for line in lines[1:]:
        name, _, value = line.partition(b":")
        headers[name.strip().lower().decode()] = value.strip().decode()
    length = int(headers.get("content-length", "0"))
    body = rest
    while len(body) < length:
        chunk = connection.recv(65536)
        if not chunk:
            break
        body += chunk
    return lines[0].decode(), headers, body[:length]


def send_response(connection, status, headers, body=b""):
    text = "HTTP/1.1 %s\r\n" % status
    for name, value in headers:
        text += "%s: %s\r\n" % (name, value)
    connection.sendall(text.encode() + b"\r\n" + body)


def query_value(target, name, default):
    for pair in target.partition("?")[2].split("&"):
        key, _, value = pair.partition("=")
        if key == name and value:
            return value
    return default


def handle_echo(connection, method, headers, body, target):
    seen = [("X-Seen-" + key.title(), value) for key, value in headers.items()]
    seen.append(("X-Seen-Target", target))
    seen.append(("X-Seen-Method", method))
    seen.append(("Content-Length", str(len(body))))
    send_response(connection, "200 OK", seen, body)


def handle_big(connection, target):
    megabytes = int(query_value(target, "mb", "1"))
    total = megabytes * 1024 * 1024
    send_response(connection, "200 OK", [("Content-Length", str(total))])
    sent = 0
    while sent < total:
        piece = PATTERN[:min(len(PATTERN), total - sent)]
        connection.sendall(piece)
        sent += len(piece)


def handle_request(connection):
    request = read_request(connection)
    if request is None:
        return
    line, headers, body = request
    method, target, _ = line.split(" ", 2)
    path = target.partition("?")[0]
    # 프록시는 원래 URI를 그대로 넘기므로 라우팅용 접두어를 여기서 뗀다
    if path.startswith("/py/"):
        path = path[3:]
    if path == "/echo":
        handle_echo(connection, method, headers, body, target)
    elif path == "/slow":
        time.sleep(float(query_value(target, "seconds", "12")))
        send_response(connection, "200 OK", [("Content-Length", "4")], b"late")
    elif path == "/slow-body":
        send_response(connection, "200 OK", [("Content-Length", "10")], b"abc")
        time.sleep(float(query_value(target, "seconds", "12")))
    elif path == "/cut-body":
        send_response(connection, "200 OK", [("Content-Length", "100")], b"only ten b")
    elif path == "/cut-header":
        connection.sendall(b"HTTP/1.1 200 OK\r\nContent-Le")
    elif path == "/cut-silent":
        pass
    elif path == "/no-length":
        send_response(connection, "200 OK", [("Content-Type", "text/plain")], b"until-close")
    elif path == "/chunked":
        send_response(connection, "200 OK", [("Transfer-Encoding", "chunked")],
                      b"5\r\nhello\r\n6\r\n world\r\n0\r\n\r\n")
    elif path == "/bad-length":
        connection.sendall(b"HTTP/1.1 200 OK\r\nContent-Length: 5\r\n"
                           b"Content-Length: 6\r\n\r\nhello!")
    elif path == "/both-length":
        connection.sendall(b"HTTP/1.1 200 OK\r\nContent-Length: 5\r\n"
                           b"Transfer-Encoding: chunked\r\n\r\nhello")
    elif path == "/garbage":
        connection.sendall(b"this is not http\r\n\r\n")
    elif path == "/switch":
        send_response(connection, "101 Switching Protocols",
                      [("Upgrade", "websocket"), ("Connection", "Upgrade")])
    elif path == "/continue":
        connection.sendall(b"HTTP/1.1 100 Continue\r\n\r\n")
        send_response(connection, "200 OK", [("Content-Length", "2")], b"ok")
    elif path == "/no-content":
        send_response(connection, "204 No Content", [("X-Marker", "yes")])
    elif path == "/head-length":
        send_response(connection, "200 OK", [("Content-Length", "50")])
    elif path == "/hop":
        send_response(connection, "200 OK",
                      [("Connection", "keep-alive, X-Hop"), ("Keep-Alive", "timeout=9"),
                       ("X-Hop", "1"), ("Content-Length", "2")], b"ok")
    elif path == "/big":
        handle_big(connection, target)
    else:
        send_response(connection, "404 Not Found", [("Content-Length", "2")], b"no")


def serve_connection(connection):
    try:
        handle_request(connection)
    except OSError:
        pass
    finally:
        try:
            connection.close()
        except OSError:
            pass


def main():
    port = int(sys.argv[1])
    listener = socket.socket()
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind(("127.0.0.1", port))
    listener.listen(1024)
    while True:
        connection, _ = listener.accept()
        threading.Thread(target=serve_connection, args=(connection,), daemon=True).start()


if __name__ == "__main__":
    main()
