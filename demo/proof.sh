#!/bin/bash
# README 주장 증명 데모 실행기. Docker 안에서 빌드하고 demo/proof.py를 돌린다.
# 사용법: demo/proof.sh            → 전부 (약 2분 반)
#         demo/proof.sh proxy security  → 고른 구역만 (발표 중 하나씩 보여 줄 때)
# 구역: memory limits cleanup sigpipe shutdown concurrency proxy security attack
# 처음 한 번은 이미지가 필요하다: docker build -t webserver-demo demo
set -e
cd "$(dirname "$0")/.."
WORK=$(pwd -W 2>/dev/null || pwd)
export MSYS_NO_PATHCONV=1
docker run --rm --ulimit nofile=65536:65536 -v "$WORK:/work" webserver-demo \
    bash -c "make -s all >/dev/null && python3 -u demo/proof.py $*"
