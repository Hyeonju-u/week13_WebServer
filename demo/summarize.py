"""wrk 출력(<loops>loop_c<연결 수>.txt)을 데모 페이지가 읽는 summary.json 하나로 모은다.
사용법: python3 demo/summarize.py demo/results > demo/results/summary.json"""
import json
import os
import re
import sys

NAME = re.compile(r"^(\d+)loop_c(\d+)\.txt$")
UNIT_MS = {"us": 0.001, "ms": 1.0, "s": 1000.0, "m": 60000.0}


def to_ms(text):
    match = re.match(r"([\d.]+)(us|ms|s|m)$", text)
    return round(float(match.group(1)) * UNIT_MS[match.group(2)], 2)


def find(pattern, text, default=None):
    match = re.search(pattern, text, re.MULTILINE)
    return match.groups() if match else default


def read_run(path):
    with open(path, encoding="utf-8") as handle:
        text = handle.read()
    requests, seconds = find(r"^\s*(\d+) requests in ([\d.]+)s", text)
    errors = find(r"Socket errors: connect (\d+), read (\d+), write (\d+), timeout (\d+)",
                  text, ("0", "0", "0", "0"))
    not_ok = int(find(r"Non-2xx or 3xx responses: (\d+)", text, ("0",))[0])
    connect, read, write, timeout = (int(value) for value in errors)
    return {
        "rps": round(float(find(r"^Requests/sec:\s+([\d.]+)", text)[0])),
        "ok": int(requests) - not_ok,
        "not_ok": not_ok,
        "timeouts": timeout,
        "socket_errors": connect + read + write,
        "p50_ms": to_ms(find(r"^\s+50%\s+(\S+)", text)[0]),
        "p99_ms": to_ms(find(r"^\s+99%\s+(\S+)", text)[0]),
        "max_ms": to_ms(find(r"^\s+Latency\s+\S+\s+\S+\s+(\S+)", text)[0]),
        "seconds": round(float(seconds), 1),
    }


def main():
    folder = sys.argv[1]
    runs = []
    for name in sorted(os.listdir(folder)):
        match = NAME.match(name)
        if match is None:
            continue
        run = read_run(os.path.join(folder, name))
        run["loops"] = int(match.group(1))
        run["connections"] = int(match.group(2))
        runs.append(run)
    runs.sort(key=lambda run: (-run["loops"], run["connections"]))
    json.dump({"tool": "wrk", "runs": runs}, sys.stdout, ensure_ascii=False, indent=1)


if __name__ == "__main__":
    main()
