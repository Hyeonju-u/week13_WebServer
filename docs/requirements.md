# 웹 서버 + Reverse 프록시 — 요구사항과 결정

> 원본: Notion "웹 서버 요구분석 (R-ID · 빈칸)" (2026-11-23 기준). 이 파일과 Notion이 다르면 알린다.
> 기준 문서: LMS "웹 서버 만들기" 과제 요구사항

## 번호 규칙

| 표시 | 뜻 |
|---|---|
| R1~R6 | 요구사항 흐름. R1a처럼 알파벳이 붙은 것이 세부 기능이고, **코드는 세부 기능 ID 하나에 연결한다** |
| C | 제약 (지켜야 할 조건) |
| Q | 품질 (검증 항목) |
| D | 큰 결정 |
| B | 세부 결정 |
| P | 파서 세부 결정 (`docs/step1-parser.md`) |
| S | 정적 응답 세부 결정 (`docs/step2-static.md`) |
| L | 이벤트 루프 세부 결정 (`docs/step3-loop.md`) |
| G | 프록시·설정 파일 세부 결정 (`docs/step4-proxy.md`) |
| E | 정상 종료 세부 결정 (`docs/step6-shutdown.md`) |

## 1. 요구사항 흐름

### R1 정적 요청
| ID | 세부 기능 |
|---|---|
| R1a | TCP 서버: socket → bind → listen → accept, `SO_REUSEADDR` |
| R1b | 요청 라인 파싱 (메서드, URI, 버전), 최소 메서드 GET·POST |
| R1c | 헤더 파싱 (Host, Content-Length, Connection 등), 헤더 끝(빈 줄) 식별 |
| R1d | 루트 디렉토리에서 파일 찾아 읽기 |
| R1e | MIME 타입 매핑 |
| R1f | 응답 생성: 상태 라인, 표준 헤더, 본문 송신 |
| R1g | 기본 오류 응답 (404 / 403 / 500 등) |

### R2 Reverse 프록시: 동적 요청을 백엔드로 전달
| ID | 세부 기능 |
|---|---|
| R2a | 본문 처리: POST 본문을 Content-Length만큼 읽기 |
| R2b | 라우팅: 경로 또는 Host 기반으로 정적 / 백엔드 결정 |
| R2c | 백엔드로 요청 전달 |
| R2d | 백엔드 응답을 받아 손님에게 전달 |
| R2e | 라운드로빈 로드 밸런싱 (지원) |

### R3 동시 접속
| ID | 세부 기능 |
|---|---|
| R3a | 멀티 이벤트 루프 (스레드 N개 × epoll N개) |
| R3b | 연결을 루프에 분배, 최대 동시 연결 수 관리 |
| R3c | 연결별 진행 상태: 부분 읽기·부분 쓰기, non-blocking |
| R3d | keep-alive (지원) |

### R4 이상한 요청
| ID | 세부 기능 |
|---|---|
| R4a | 잘못된 요청: 불완전한 헤더, 비정상 메서드, 잘못된 버전 |
| R4b | 크기 상한: 긴 URI, 큰 본문, 버퍼 오버플로우 방지 |
| R4c | 루트 밖 경로 차단 → 403 |

### R5 Reverse 프록시 장애
| ID | 세부 기능 |
|---|---|
| R5a | 백엔드 접속 실패 → 502 |
| R5b | 백엔드 응답 없음, 타임아웃 → 504 |
| R5c | 죽은 백엔드 건너뛰기 |

### R6 끊김과 종료
| ID | 세부 기능 |
|---|---|
| R6a | 손님 끊김·비정상 종료 시 자원 회수 |
| R6b | SIGPIPE 무시 |
| R6c | SIGINT / SIGTERM 안전한 셧다운 |

## 2. 제약

| ID | 내용 |
|---|---|
| C1 | 언어 C |
| C2 | 완성된 HTTP / 이벤트 루프 라이브러리 금지. 표준 C, POSIX API만 |
| C3 | Makefile 또는 CMakeLists.txt |
| C4 | 포트, 정적 루트, 라우팅 규칙을 설정으로 받음 |
| C5 | Linux (WSL2 / Docker) |

## 3. 품질

| ID | 내용 |
|---|---|
| Q1 | 단위 테스트 (HTTP 파서, MIME, 라우팅/프록시 전달) |
| Q2 | 기능 테스트 (curl, 브라우저) |
| Q3 | 동시 다중 접속 정상 동작 |
| Q4 | 부하 테스트 (wrk) |
| Q5 | 누수 검사 (valgrind / ASan) |
| Q6~Q9 | 엣지: 잘못된 요청, 큰 본문·긴 URI, 백엔드 무응답, 갑작스러운 끊김 |
| Q10~Q11 | 포트폴리오 수준 완성도, README 기준 발표에 검증 과정 포함 |
| Q12 | 동시 연결 10,000개 유지 (C10K, 과제 "중점 포인트"). 3단계는 설계로 대비(epoll, 연결당 메모리 고정, backlog `SOMAXCONN`), 측정은 7단계 `wrk -c 10000` (fd 한도 20,018 이상 환경) |

## 4. 큰 결정 (D)

| # | 결정 |
|---|---|
| D1 | Reverse 프록시 |
| D2 | 웹 서버 + 프록시 합침. 정적은 직접 응답, 동적만 백엔드로 |
| D3 | 멀티 이벤트 루프 (스레드 N개 × epoll N개) |
| D4 | 백엔드는 파이썬 간단 서버 |
| D5 | 리눅스 전용 (epoll) |
| D6 | accept 전담 스레드 + 루프별 넘겨받기 큐 + `eventfd` 벨. 루프는 큐에서 꺼내 자기 epoll·타임아웃 목록에 직접 등록 |
| D7 | keep-alive 지원 |
| D8 | 백엔드 응답은 다 받고 한 번에 전달 + 응답 크기 상한 |
| D9 | 시그널은 전용 스레드가 `sigwait`. 스레드 생성 전에 `pthread_sigmask`로 막기 |
| D10 | 설정 파일 (4단계 G15에서 만듦) |

## 5. 정해진 세부 결정 (B)

| # | 결정 |
|---|---|
| B1 | HTTP/1.1만. 그 외(1.0 포함) 505 |
| B2 | Host 없으면 400. 절대 주소 형식이어도 Host 헤더 필요 (RFC 9112 3.2) |
| B3 | `Transfer-Encoding` 있으면 501, POST인데 Content-Length 없으면 411, Content-Length가 숫자가 아니면 400 |
| B4 | 요청 라인 8KB 초과 414, 헤더 전체 8KB 초과 431, 본문 1MB 초과 413. 경계값은 허용(8,192 / 1,048,576바이트까지 OK). 나중에 설정 파일로 변경 가능하게 |
| B5 | GET, POST만. 그 외(HEAD 포함) 501 |
| B6 | 단위 테스트는 직접 만든 검사 매크로 (실패해도 계속, 마지막에 통과/실패 개수) |
| B13 | 빌드 도구는 Makefile (`make test`, `make asan`) |
| B29 | `\r` 없는 `\n`은 400 |
| B30 | Content-Length 중복이면 값과 무관하게 400 |
| B31 | BAD 응답 뒤에는 항상 `Connection: close`로 답하고 연결 닫기 |
| B32 | 에러가 여러 개면 먼저 걸린 것 (검사 순서 = 우선순위) |
| B7 | 정적 요청의 쿼리는 버림 (S1) |
| B8 | 디렉토리: index.html, 없으면 403, 슬래시 없으면 301 (S9, S10) |
| B9 | MIME 표와 모르는 확장자 octet-stream (S14~S17) |
| B10 | 루트 밖: 글자로 `.`·`..` 정리 후 realpath 경계 비교, 403 (S6) |
| B11 | 파일 오류 매핑, 그 밖 500 (S7) |
| B12 | 정적 POST는 405 + `Allow: GET` (S8) |
| B14 | epoll은 level-triggered (L3) |
| B15 | 손님 쪽 타임아웃: 헤더 전체 60초 / 본문 사이 60초 / 쓰기 사이 60초 / keep-alive idle 75초 (L5, L6) |
| B16 | 최대 연결 수 = fd 소프트 한도 − 여유분, 넘으면 listen을 잠깐 뺌 (L8). 4단계에서 ÷ 2 (G19) |
| B17 | 라우팅은 경로 기반 (G1) |
| B18 | 경로 조각 경계 접두사, 먼저 적힌 규칙 우선, 안 맞으면 정적 (G2) |
| B19 | 경로는 떼지 않고 원문 그대로 전달 (G3, G4) |
| B20 | hop-by-hop·Expect 지움, Host 유지, X-Forwarded-For 덧붙임 (G6) |
| B21 | 요청마다 새 연결, HTTP/1.0 + Connection: close (G5) |
| B22 | least_conn(같으면 라운드로빈) 또는 round_robin을 규칙마다 설정 (G14) |
| B23 | 백엔드 connect 5초, 쓰기 사이 60초, 읽기 사이 60초 (G11) |
| B24 | 실패 1번이면 10초 빼기, connect 실패만 재시도, 전부 빠지면 한 번은 시도 (G13) |
| B28 | 백엔드 응답 상한 기본 8MB, 넘으면 502. 요청 본문 max_body 설정 가능 (G9) |
| B25 | 루프 개수: 5단계 건너뜀 (2026-11-25). 루프 1개로 완성, 멀티 루프는 README 한계·설계로 남김 |
| B26 | 공유 데이터 원자적 처리: 5단계 건너뜀. 바뀌는 값은 `upstream_t` 한 곳에 모아 둠 (G29) |
| B27 | 정상 종료 마감 기본 5초, `shutdown_timeout`으로 설정 (E5) |

B25·B26은 5단계를 건너뛰어 README 한계로, B27은 `docs/step6-shutdown.md`의 E 결정. B7~B12의 자세한 내용은 `docs/step2-static.md`의 S 결정, B14~B16은 `docs/step3-loop.md`의 L 결정, B17~B24·B28은 `docs/step4-proxy.md`의 G 결정. 필요하면 멈추고 질문할 것.

## 6. 비목표

- Forward 프록시, `CONNECT`, CGI, HTTPS
- 백엔드 응답 흘려보내기 (D8)
- 응답 `Transfer-Encoding: chunked`, Range, 조건부 요청, 인증, 프록시 캐시
- HTTP/1.0, HTTP/2, HTTP/3
- HEAD (RFC 7231은 MUST → README 한계에 기록)
- GET, POST 외 메서드의 실제 처리
