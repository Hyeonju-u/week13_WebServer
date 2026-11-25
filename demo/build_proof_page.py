"""증명 결과(proof.json, slowhttptest CSV, wrk summary.json)를 페이지 틀에 넣어
혼자 열리는 demo/results/proof.html을 만든다.
사용법: python3 demo/build_proof_page.py"""
import csv
import json
import os

HERE = os.path.dirname(os.path.abspath(__file__))
RESULTS = os.path.join(HERE, "results")


def read_csv(name):
    path = os.path.join(RESULTS, "proof", name + ".csv")
    if not os.path.exists(path):
        return []
    with open(path) as handle:
        return [{"t": int(row["Seconds"]), "connected": int(row["Connected"]),
                 "closed": int(row["Closed"]), "up": row["Service Available"] != "0"}
                for row in csv.DictReader(handle)]


def read_json(path):
    if not os.path.exists(path):
        return None
    with open(path, encoding="utf-8") as handle:
        return json.load(handle)


def main():
    data = {
        "proof": read_json(os.path.join(RESULTS, "proof", "proof.json")),
        "slowloris": read_csv("slowloris"),
        "slowread": read_csv("slowread"),
        "load": read_json(os.path.join(RESULTS, "summary.json")),
    }
    with open(os.path.join(HERE, "proof_template.html"), encoding="utf-8") as handle:
        page = handle.read().replace("__DATA__", json.dumps(data, ensure_ascii=False))
    page = '<!doctype html><meta charset="utf-8">\n<meta name="viewport" content="width=device-width, initial-scale=1">\n' + page
    with open(os.path.join(RESULTS, "proof.html"), "w", encoding="utf-8") as handle:
        handle.write(page)


if __name__ == "__main__":
    main()
