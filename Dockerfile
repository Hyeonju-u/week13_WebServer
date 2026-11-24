# (C5) 리눅스 빌드·테스트 환경. 소스는 이미지에 넣지 않고 실행할 때 붙인다
#   docker build -t webserver-dev .
#   docker run --rm -v "${PWD}:/work" webserver-dev make test
#   docker run --rm -v "${PWD}:/work" webserver-dev make asan
#   docker run --rm -v "${PWD}:/work" webserver-dev make itest
#   docker run --rm -v "${PWD}:/work" webserver-dev make itest-asan
FROM ubuntu:24.04

# (L4, L41) python3: 통합 테스트, curl: 완료 기준 확인과 README 시연
RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        build-essential \
        valgrind \
        python3 \
        curl \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /work

# (S18, S31) root는 권한 000 파일도 열어서 O-4·D-6을 검증할 수 없다.
# ubuntu:24.04에 이미 있는 uid 1000 사용자를 쓴다
USER ubuntu

CMD ["make", "test"]
