"""증명 기록과 wrk 결과를 슬라이드 덱 틀에 넣어 혼자 열리는 demo/results/deck.html을 만든다.
사용법: python3 demo/build_deck.py"""
import json
import os

from build_proof_page import RESULTS, read_csv, read_json

HERE = os.path.dirname(os.path.abspath(__file__))


def main():
    data = {
        "proof": read_json(os.path.join(RESULTS, "proof", "proof.json")),
        "slowloris": read_csv("slowloris"),
        "slowread": read_csv("slowread"),
        "load": read_json(os.path.join(RESULTS, "summary.json")),
        "scaling": read_json(os.path.join(RESULTS, "scaling.json")),
    }
    with open(os.path.join(HERE, "deck_template.html"), encoding="utf-8") as handle:
        page = handle.read().replace("__DATA__", json.dumps(data, ensure_ascii=False))
    page = '<!doctype html><meta charset="utf-8">\n<meta name="viewport" content="width=device-width, initial-scale=1">\n' + page
    with open(os.path.join(RESULTS, "deck.html"), "w", encoding="utf-8") as handle:
        handle.write(page)


if __name__ == "__main__":
    main()
