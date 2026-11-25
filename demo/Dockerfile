# 증명 데모용 이미지: 테스트 이미지(webserver-stable)에 외부 공격 도구 slowhttptest를 더한다.
# 만들기: docker build -t webserver-demo demo
FROM webserver-stable
USER root
RUN apt-get update \
    && apt-get install -y --no-install-recommends slowhttptest \
    && rm -rf /var/lib/apt/lists/*
USER ubuntu
WORKDIR /work
