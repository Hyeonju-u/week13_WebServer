# 6단계: 정상 종료 (graceful shutdown)

기준: LMS "웹 서버 만들기" 과제, 설계 페이지 6단계 완료 기준 "Ctrl+C 시 처리 중 요청 마무리 후 종료".
5단계(멀티 루프)는 건너뜀 (2026-11-25). 그래서 이 단계는 **루프 1개** 기준이다.

## 1. 범위

- 6단계 계획 중 이미 된 것: SIGPIPE 무시(`main.c`), 최대 연결 수(L8, G19), 크기 상한 413/414/431(P, S).
- graceful close(연결 하나를 lingering으로 닫기)는 3단계 `start_lingering`/`read_lingering`(L6, L12, B31)에서 끝남.
- 이 단계에서 하는 것: 서버 전체의 정상 종료.

## 2. 정한 결정 (E, 2026-11-25)

| # | 결정 | 근거 | 대가 / 남는 구멍 | R |
|---|---|---|---|---|
| E1 | 신호는 `signalfd`로 받아 epoll에 등록. 루프 시작 전에 `sigprocmask`로 SIGINT·SIGTERM을 막아 둠 | 신호가 루프 안의 평범한 이벤트가 되어 그 자리에서 `free`·`fflush`·로그를 쓸 수 있음. 핸들러 + 플래그 방식은 확인 직후 온 신호를 epoll_wait이 최대 keepalive(75초)까지 놓치는 틈이 있음 | 리눅스 전용 (Docker 리눅스라 문제없음) | — |
| E2 | SIGINT(Ctrl+C)와 SIGTERM(`docker stop`, `kill`) 둘 다 정상 종료. 정상 종료 중 신호가 한 번 더 오면 마감을 지금으로 당김 (E5의 마감 처리를 바로 실행) | 완료 기준이 Ctrl+C 정상 종료. docker stop은 SIGTERM을 보내므로 TERM도 처리해야 Docker에서 정상 종료가 일어남. "지금으로 당김"은 마감 경로 재사용이라 정리(누수 0, 로그 `fflush`)를 그대로 함 | 실수로 두 번 누르면 처리 중 응답이 잘림. 손님은 `Content-Length` 부족으로 잘린 걸 알아챔. SIGHUP은 처리 안 함(기본 동작), SIGKILL은 막을 수 없음 → README | — |
| E3 | 종료가 시작되면 listen 소켓을 바로 닫음. 새 connect는 커널이 거절 | nginx와 같음. 종료 중 accept·응답 비용을 안 씀 | 백로그에 있던 accept 전 연결은 RST로 끊김 → README | — |
| E4 | 상태별 처리: `CONN_IDLE`과 1바이트도 안 온 `CONN_READ_HEAD`는 바로 닫음. 일부가 온 `CONN_READ_HEAD`, 본문 받는 중, 프록시 상태들(G26), `CONN_SEND`는 마감까지 끝내게 두고 응답에 `Connection: close`. `CONN_LINGER`, `CONN_DISCARD`는 마감까지 그대로 | 진행 중인 요청이 없는 연결은 기다릴 이유가 없음. 요청을 보내는 중인 손님을 끊으면 완료 기준에 어긋남 | 느린 손님(slowloris)이 마감까지 종료를 붙잡음 → E5. 종료 중 lingering은 linger_total(30초)이 아니라 마감(5초)에서 잘림 | — |
| E5 | 마감 기본 5초, 설정 파일 `shutdown_timeout`으로 바꿈 (B27). 마감이 되면 남은 카드마다 `destroy_conn`(→ `release_backend`), epoll·signalfd 닫기, 설정·`upstream_t` free, 접근 로그 `fflush` 후 닫기, `main`에서 return | docker stop의 10초(이후 SIGKILL) 안에 정리를 끝냄. 무제한이면 느린 손님 하나가 종료를 막음 | 5초 넘게 걸리는 요청(큰 파일, `/api/slow`)은 잘림. 백엔드가 처리한 POST의 응답이 잘리면 손님은 처리 여부를 모름 → README (실서비스는 idempotency key) | — |

| E6 | 종료 코드: 정상 종료는 마감 전이든 마감·두 번째 신호로 끝나든 전부 0. 설정 오류 2, 환경 오류 1 (G31) | 사람이 요청한 종료는 실패가 아님. 0이 아니면 Docker·systemd 재시작 정책이 엉뚱하게 반응 | 강제로 닫은 연결이 있었는지는 종료 코드로 못 봄 → E7 로그로 봄 | — |
| E7 | 종료 로그: stderr에 `shutdown: start (active N)`, `shutdown: done (forced M)`. M = 마감으로 강제로 닫은 연결 수 | 처리 중 요청을 마무리했는지(M=0) 시연·테스트에서 바로 확인 | — | — |

## 3. 테스트 목록 (E8, Python 통합 테스트)

| ID | 상황 | 기대 결과 | 결정 |
|---|---|---|---|
| SD-1 | 손님 없음 + SIGINT | 바로 종료, 코드 0, `forced 0` | E1, E2, E6, E7 |
| SD-2 | 큰 파일을 받는 중 + SIGTERM | 응답 끝까지 받음, 그 뒤 종료, `forced 0` | E2, E4 |
| SD-3 | `/api/slow?ms=2000` 처리 중 + SIGINT | 200을 받음, 백엔드 처리 중 수 0, 종료 | E4, G26 |
| SD-4 | keep-alive idle 연결 + SIGINT | 그 연결은 바로 닫히고 바로 종료 | E4 |
| SD-5 | 종료 시작 뒤 새 connect | 거절됨 | E3 |
| SD-6 | `/api/slow?ms=20000` + `shutdown_timeout 1s` / 같은 상황에서 신호 두 번 | 1초 뒤 종료, `forced 1` / 바로 종료, `forced 1` | E2, E5 |
| SD-7 | ASan 빌드로 SD-1~6 | 잘못된 접근 0, 누수 0 | E5 |
