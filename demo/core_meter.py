"""서버 코어마다 사용률을 막대로 그린다. wrk는 끝날 때까지 화면에 아무것도 안 내므로,
그동안 서버 코어가 몇 개나 일하는지 보여 주려고 쓴다. /proc/stat을 0.5초마다 읽는다.
사용법: python3 -u demo/core_meter.py <코어 범위, 예 0-8> <초> [제목]"""
import sys
import time

BAR_WIDTH = 40
INTERVAL = 0.5


def parse_cores(text):
    first, _, last = text.partition("-")
    return list(range(int(first), int(last or first) + 1))


def read_cpu_times():
    times = {}
    with open("/proc/stat") as handle:
        for line in handle:
            name, *values = line.split()
            if not name.startswith("cpu") or name == "cpu":
                continue
            numbers = [int(value) for value in values]
            idle = numbers[3] + numbers[4]
            times[int(name[3:])] = (sum(numbers), idle)
    return times


def compute_usage(before, after, core):
    total = after[core][0] - before[core][0]
    idle = after[core][1] - before[core][1]
    return 0.0 if total <= 0 else max(0.0, 1.0 - idle / total)


def draw(cores, usages, elapsed, seconds, title):
    lines = ["\033[H\033[2J", "%s   %4.1f초 / %d초" % (title, elapsed, seconds), ""]
    busy = 0
    for core in cores:
        usage = usages[core]
        busy += usage
        filled = int(round(usage * BAR_WIDTH))
        color = "\033[32m" if usage < 0.5 else "\033[33m" if usage < 0.85 else "\033[31m"
        lines.append("  코어 %2d  %s%s\033[0m%s %3d%%" % (
            core, color, "█" * filled, "·" * (BAR_WIDTH - filled), round(usage * 100)))
    lines.append("")
    lines.append("  일하는 코어 합계: %.1f개 / %d개" % (busy, len(cores)))
    sys.stdout.write("\n".join(lines) + "\n")
    sys.stdout.flush()


def main():
    cores = parse_cores(sys.argv[1])
    seconds = int(sys.argv[2])
    title = sys.argv[3] if len(sys.argv) > 3 else "서버 코어 사용률"
    start = time.time()
    before = read_cpu_times()
    while time.time() - start < seconds:
        time.sleep(INTERVAL)
        after = read_cpu_times()
        usages = {core: compute_usage(before, after, core) for core in cores}
        draw(cores, usages, time.time() - start, seconds, title)
        before = after


if __name__ == "__main__":
    main()
