#!/usr/bin/env python3
"""죽은 백엔드 기억 테스트: 연결에 실패한 백엔드를 backend_fail_timeout 동안
Round-Robin 후보에서 빼는지 확인한다 (PR #2의 LB-4~6을 옮김)."""
import os
import shutil
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import Checker, Server, find_free_port, get

checker = Checker()
check = checker.check
FAIL_SECONDS = 2
CONNECT_FAILED_LOG = "백엔드 연결 실패"
# get()은 응답 뒤 서버가 닫는지 1초 기다린다. 닫게 해야 10번이 빼는 시간 안에 끝난다
CLOSE = "Connection: close\r\n"


def prepare_root(base, name):
    root = os.path.join(base, name)
    for route in ("api", "both"):
        os.makedirs(os.path.join(root, route))
        with open(os.path.join(root, route, "who.txt"), "w") as handle:
            handle.write(name)
    return root


def count_connect_failures(proxy):
    return proxy.log_text().count(CONNECT_FAILED_LOG)


def test_skip_dead_backend(proxy, live_name):
    """LB-4: 하나를 끄고 10번 보내면 전부 200이고, 연결 실패(재시도)는 첫 요청 한 번뿐"""
    print("[하나 끄고 10번]")
    responses = [get(proxy.port, "/api/who.txt", CLOSE) for _ in range(10)]
    check("전부 200", all(r is not None and r.status == 200 for r in responses))
    check("전부 살아 있는 백엔드가 응답",
          all(r is not None and r.body == live_name.encode() for r in responses))
    failures = count_connect_failures(proxy)
    check("죽은 백엔드 연결 시도는 첫 요청 한 번뿐 (%d번)" % failures, failures == 1)


def test_rejoin_after_timeout(proxy, base, dead_port, processes):
    """LB-5: 빼는 시간이 지난 뒤 죽었던 백엔드를 켜면 다시 분배 받는다"""
    print("[빼는 시간이 지나고 켬]")
    time.sleep(FAIL_SECONDS + 0.5)
    revived = Server(prepare_root(base, "revived"), port=dead_port)
    processes.append(revived)
    bodies = set()
    for _ in range(6):
        response = get(proxy.port, "/api/who.txt", CLOSE)
        if response is not None and response.status == 200:
            bodies.add(response.body)
    check("되살아난 백엔드도 다시 받음 (%s)" % sorted(bodies), b"revived" in bodies)
    check("원래 살아 있던 백엔드도 계속 받음", b"live" in bodies)


def test_all_dead_then_one_back(proxy, base, both_ports, processes):
    """LB-6: 둘 다 끄면 502, 둘 다 실패한 직후 하나를 켜면 빼는 시간 안이라도 성공"""
    print("[둘 다 끔]")
    response = get(proxy.port, "/both/who.txt", CLOSE)
    check("둘 다 꺼져 있으면 502", response is not None and response.status == 502)
    back = Server(prepare_root(base, "back"), port=both_ports[1])
    processes.append(back)
    response = get(proxy.port, "/both/who.txt", CLOSE)
    check("둘 다 실패한 직후 하나를 켜면 200",
          response is not None and response.status == 200 and response.body == b"back")


def find_distinct_ports(count, taken):
    ports = []
    while len(ports) < count:
        port = find_free_port()
        if port not in taken and port not in ports:
            ports.append(port)
    return ports


def write_config(base, dead_port, live_port, both_ports):
    path = os.path.join(base, "proxy.conf")
    with open(path, "w") as handle:
        handle.write("route /api 127.0.0.1:%d 127.0.0.1:%d\n" % (dead_port, live_port))
        handle.write("route /both 127.0.0.1:%d 127.0.0.1:%d\n" % both_ports)
        handle.write("connect_timeout 1\nresponse_timeout 2\n")
        handle.write("backend_fail_timeout %d\n" % FAIL_SECONDS)
    return path


def main():
    base = tempfile.mkdtemp(prefix="ws_health_")
    processes = []
    try:
        live = Server(prepare_root(base, "live"))
        processes.append(live)
        dead_port, first_dead, second_dead = find_distinct_ports(3, {live.port})
        both_ports = (first_dead, second_dead)
        config = write_config(base, dead_port, live.port, both_ports)
        proxy = Server(prepare_root(base, "proxy"), config=config, log_level="debug")
        processes.append(proxy)

        test_skip_dead_backend(proxy, "live")
        test_rejoin_after_timeout(proxy, base, dead_port, processes)
        test_all_dead_then_one_back(proxy, base, both_ports, processes)

        print("[종료]")
        for server in processes:
            code = server.stop()
            check("서버 %d SIGINT로 정상 종료(코드 0)" % server.port, code == 0, "code=%s" % code)
    finally:
        for process in processes:
            process.stop()
        shutil.rmtree(base, ignore_errors=True)
    return checker.finish()


if __name__ == "__main__":
    sys.exit(main())
