"""scaling.sh의 wrk 출력(loops<N>.txt)을 확장 곡선 JSON으로 모은다.
사용법: python3 demo/summarize_scaling.py demo/results/scaling > demo/results/scaling.json"""
import json
import os
import re
import sys

from summarize import find, read_run

NAME = re.compile(r"^loops(\d+)\.txt$")


def main():
    folder = sys.argv[1]
    points = []
    connections = None
    for name in os.listdir(folder):
        match = NAME.match(name)
        if match is None:
            continue
        path = os.path.join(folder, name)
        run = read_run(path)
        with open(path, encoding="utf-8") as handle:
            text = handle.read()
        busy = find(r"^busy_cores ([\d.]+)", text, ("0",))[0]
        connections = int(find(r"(\d+) connections", text, ("0",))[0])
        run["loops"] = int(match.group(1))
        run["busy_cores"] = float(busy)
        points.append(run)
    points.sort(key=lambda point: point["loops"])
    base = points[0]["rps"] / points[0]["loops"] if points else 1
    for point in points:
        point["speedup"] = round(point["rps"] / points[0]["rps"], 2)
        point["ideal_rps"] = round(base * point["loops"])
        point["rps_per_busy_core"] = round(point["rps"] / point["busy_cores"]) if point["busy_cores"] else None
    json.dump({"tool": "wrk", "connections": connections, "points": points}, sys.stdout,
              ensure_ascii=False, indent=1)


if __name__ == "__main__":
    main()
