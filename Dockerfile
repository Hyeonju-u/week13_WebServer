# (C5) 리눅스 빌드·테스트 환경. 소스는 이미지에 넣지 않고 실행할 때 붙인다
#   docker build -t webserver-dev .
#   docker run --rm -v "${PWD}:/work" webserver-dev make test
#   docker run --rm -v "${PWD}:/work" webserver-dev make asan
FROM ubuntu:24.04

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        build-essential \
        valgrind \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /work
CMD ["make", "test"]
