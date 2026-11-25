"""summary.json을 페이지 틀에 넣어 혼자 열리는 demo/results/demo.html을 만든다.
사용법: python3 demo/build_page.py"""
import os

HERE = os.path.dirname(os.path.abspath(__file__))


def main():
    with open(os.path.join(HERE, "page_template.html"), encoding="utf-8") as handle:
        template = handle.read()
    with open(os.path.join(HERE, "results", "summary.json"), encoding="utf-8") as handle:
        data = handle.read()
    scaling_path = os.path.join(HERE, "results", "scaling.json")
    scaling = "null"
    if os.path.exists(scaling_path):
        with open(scaling_path, encoding="utf-8") as handle:
            scaling = handle.read()
    page = template.replace("__DATA__", data).replace("__SCALING__", scaling)
    # 파일로 바로 열 때도 한글이 깨지지 않게 문서 틀을 붙인다
    page = '<!doctype html><meta charset="utf-8">\n<meta name="viewport" content="width=device-width, initial-scale=1">\n' + page
    with open(os.path.join(HERE, "results", "demo.html"), "w", encoding="utf-8") as handle:
        handle.write(page)


if __name__ == "__main__":
    main()
