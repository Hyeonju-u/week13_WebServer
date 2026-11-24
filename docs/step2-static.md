# 2단계: 정적 응답 (경로 → 파일, 응답 헤더)

> 결정: 2026-11-24 대화 (Notion "웹 서버 요구분석"에 같은 내용 기록)
> 관련 R: R1d, R1e, R1f, R1g, R4c
> 완료 기준: 아래 "테스트 목록"이 전부 통과하고, ASan 빌드에서 누수·잘못된 접근이 0. 실제 소켓 전송은 3단계에서.

## 1. 범위

- **한다:** 경로 정리(`?` 자르기, `%` 풀기, `.`·`..` 정리), 루트 밖 차단, 파일 찾기와 열기, 디렉토리 처리, MIME 고르기, 응답 헤더 만들기, 오류 응답 본문
- **안 한다:** 소켓 읽기·쓰기, 파일 내용 읽어서 보내기(3단계), 라우팅(정적인지 동적인지 가르기, 4단계), 설정 파일(X-2)
- 이 단계의 코드는 **소켓을 모른다.** 입력은 1단계의 `request_t`(경로, 메서드, keep_alive)와 정적 루트 경로다.
- 이 단계는 "이미 정적으로 분류된 요청"을 받는다고 본다.

## 2. 흐름

```
request_t (path = "/docs/a%20b.html?x=1", method, keep_alive)
  → ① ? 뒤 자르기                      "/docs/a%20b.html"           (S1)
  → ② % 한 번 풀기                      "/docs/a b.html"             (S2~S5)
  → ③ . / .. 글자로 정리, 루트 위면 403   "/docs/a b.html"             (S6)
  → ④ 루트와 붙이고 파일 확인            404 / 403 / 500              (S6, S7)
  → ⑤ 디렉토리면 301 또는 index.html     403                          (S9, S10)
  → ⑥ 열기 + 크기 알기 (open + fstat)    fd, size                     (S11)
  → ⑦ MIME 고르기                       "text/html; charset=utf-8"   (S14~S17)
  → ⑧ 응답 헤더 만들기                   "HTTP/1.1 200 OK\r\n...\r\n\r\n" (S12)
  (오류면 ⑧에서 오류 본문까지 만든다 (S13))
```

## 3. 검사 순서 (= 에러 우선순위, B32와 같은 방식)

먼저 걸린 에러에서 바로 반환한다 (early return).

| 순서 | 검사 | 실패 | 결정 |
|---|---|---|---|
| 1 | `?` 뒤를 자른다 (쿼리 안의 내용은 검사하지 않음) | — | S1 |
| 2 | `%` 풀기: 앞에서부터 보고 처음 걸린 것. 깨진 인코딩 / `%00` / `%2F` | 400 / 400 / 404 | S3, S4, S5 |
| 3 | `.`·`..` 정리 중 루트 위로 올라감 | 403 | S6 |
| ❓ | 정적 경로에 POST (어디에 둘지 미정, 아래 7번 Q-A) | 405 | S8 |
| 4 | 루트와 붙인 경로 확인: 없음 / 권한 없음 / 그 밖 | 404 / 403 / 500 | S7 |
| 5 | 실제 위치(`realpath`)가 루트 밖 (심볼릭 링크) | 403 | S6 |
| 6 | 디렉토리: `/`로 안 끝남 → 301, index.html 없음 → 403 (index.html도 4·5·7번 검사를 받음) | 301 / 403 | S9, S10 |
| 7 | 일반 파일·디렉토리가 아님 (FIFO, 장치 파일 등) | 403 | S7 |
| 8 | 열기 실패: 권한 / 그 밖 | 403 / 500 | S7 |

- FIFO는 `open`하면 멈출 수 있다. 7번 검사가 열기보다 먼저 와야 한다 (O-5가 확인).

## 4. MIME 표 (S14)

| 확장자 | 타입 |
|---|---|
| `html`, `htm` | `text/html; charset=utf-8` |
| `css` | `text/css; charset=utf-8` |
| `js` | `text/javascript; charset=utf-8` |
| `txt` | `text/plain; charset=utf-8` |
| `json` | `application/json` |
| `png` | `image/png` |
| `jpg`, `jpeg` | `image/jpeg` |
| `gif` | `image/gif` |
| `svg` | `image/svg+xml` |
| `ico` | `image/x-icon` |
| `webp` | `image/webp` |
| `pdf` | `application/pdf` |
| `woff2` | `font/woff2` |
| `mp4` | `video/mp4` |
| 그 밖, 확장자 없음 | `application/octet-stream` |

- 확장자는 경로의 **마지막 조각**(마지막 `/` 뒤)에서 **마지막 `.` 뒤**. 대소문자 무시 (S15)
- charset은 text 계열에만 (S16)

## 5. 응답 (S12, S13)

- 상태 줄 `HTTP/1.1 <코드> <사유>`
- 항상: `Content-Type`, `Content-Length`, `Connection`(`keep-alive` / `close`), `Date`(IMF-fixdate, 예 `Sun, 06 Nov 1994 08:49:37 GMT`)
- 405에 `Allow: GET`, 301에 `Location`
- `Server` 헤더는 넣지 않는다
- 200이 아닌 응답은 본문이 코드별 고정 짧은 HTML (`text/html; charset=utf-8`). 301 본문은 이동할 주소 링크를 포함
- 다루는 코드와 사유 문구: 200 OK, 301 Moved Permanently, 400 Bad Request, 403 Forbidden, 404 Not Found, 405 Method Not Allowed, 411 Length Required, 413 Content Too Large, 414 URI Too Long, 431 Request Header Fields Too Large, 500 Internal Server Error, 501 Not Implemented, 505 HTTP Version Not Supported

## 6. 수명 규칙

- ⑥에서 연 파일 fd는 결과를 받은 쪽이 소유한다. 3단계에서는 연결의 진행 카드가 들고 있다가 응답이 끝나면 닫는다.
- 오류로 끝나는 모든 경로에서 연 fd는 닫힌다 (L-2).
- 정리 함수는 두 번 불려도 안전하게.

## 7. 아직 정하지 않은 것

### 사람이 정할 것 (Claude Code는 이 항목을 만나면 멈추고 묻는다)
| # | 질문 |
|---|---|
| Q-A | 정적 POST 405를 검사 순서 어디에 둘지 (예: `POST /없는파일`이 405인지 404인지) |
| Q-B | 경로가 너무 길어 붙이면 OS 한계를 넘을 때(`ENAMETOOLONG`), 심볼릭 링크가 고리를 이룰 때(`ELOOP`)의 코드 |
| Q-C | 301 `Location`에 쓸 경로: 손님이 보낸 원문(인코딩된 상태) + `/` 인지, 푼 경로를 다시 인코딩한 것인지 |
| Q-D | `Date`를 5xx 응답에도 넣을지 (RFC는 MAY) |

### Claude Code가 질문 목록으로 가져올 것 (인터페이스)
- 함수 이름과 시그니처, 결과 구조체(상태 코드, fd, 크기, MIME, Location 등) 필드
- 정적 루트를 어떻게 넘길지 (시작할 때 `realpath` 한 번 / 매번)
- 응답 헤더를 쓸 버퍼의 크기와 모자랄 때 동작
- `Date` 테스트를 위해 현재 시각을 어떻게 넣을지 (테스트에서 고정 시각을 써야 함)
- 파일 구성과 Makefile 변경

## 8. 테스트 목록

- 테스트 이름이나 주석에 아래 ID를 적는다 (예: `/* T-3 (S6) */`)
- 표에 없는 케이스는 **추가하지 말고 제안만** 한다
- 테스트 파일은 실행 중에 `/tmp` 아래 임시 디렉토리에 만들고 끝나면 지운다 (S18). 루트는 그 안의 `www/`

### U. 경로 정리
| # | 입력 경로 | 기대 | 결정 |
|---|---|---|---|
| U-1 | `/a%20b.html` | 파일 `a b.html`을 찾음 | S2 |
| U-2 | `/%ED%95%9C.html` | 파일 `한.html`을 찾음 | S2 |
| U-3 | `/a.html?x=1` | 파일 `a.html`을 찾음 | S1 |
| U-4 | `/a.html?x=%zz` | 200 (쿼리는 먼저 버려서 검사 안 함) | S1, S3 |
| U-5 | `/%61.html` / `/%2E%2e` 대소문자 섞인 16진수 | `a.html` / `..`로 풂 | S2 |
| U-6 | `/a%zz.html` | 400 | S3 |
| U-7 | `/a%2` (끝에서 한 자리) / `/a%` | 400 / 400 | S3 |
| U-8 | `/secret.txt%00.html` | 400 | S4 |
| U-9 | `/a%2Fb.html` / `/a%2fb.html` (`a/b.html` 파일이 실제로 있어도) | 404 / 404 | S5 |
| U-10 | `/%252e` | 한 번만 풀어서 이름이 `%2e`인 파일을 찾음 | S2 |
| U-11 | `/a+b.html` | 파일 `a+b.html`을 찾음 (`+`는 공백 아님) | S2 |

### T. 루트 밖 차단
| # | 입력 / 준비 | 기대 | 결정 |
|---|---|---|---|
| T-1 | `/a/../b.html` | `b.html` 200 | S6 |
| T-2 | `/./b.html` | `b.html` 200 | S6 |
| T-3 | `/../etc/passwd` | 403 | S6 |
| T-4 | `/a/../../b.html` | 403 | S6 |
| T-5 | `/%2e%2e/secret` (풀면 `..`) | 403 | S2, S6 |
| T-6 | `/../없는파일` | 403 (404 아님) | S6 |
| T-7 | 루트 안 링크 → 루트 밖 파일 | 403 | S6 |
| T-8 | 루트 안 링크 → 루트 안 파일 | 200 | S6 |
| T-9 | 루트 `.../www`, 링크 → 이웃 `.../www2/f.html` | 403 (접두어만 같은 경로를 안으로 보지 않음) | S6 |
| T-10 | `/a..b.html` (이름에 `..`가 들어감) | 일반 이름으로 처리 (있으면 200) | S6 |

### O. 열기와 오류 매핑
| # | 준비 | 기대 | 결정 |
|---|---|---|---|
| O-1 | 있는 파일 | 200, fd 유효, 크기 = 파일 크기 | S11 |
| O-2 | 없는 파일 | 404 | S7 |
| O-3 | `/file.html/x` (파일 밑을 디렉토리처럼) | 404 | S7 |
| O-4 | 권한 000 파일 | 403 | S7 |
| O-5 | 루트 안 FIFO (`mkfifo`) | 403, 멈추지 않음 | S7 |

### D. 디렉토리
| # | 입력 / 준비 | 기대 | 결정 |
|---|---|---|---|
| D-1 | `/` + `www/index.html` 있음 | index.html 200 | S10 |
| D-2 | `/docs/` + `docs/index.html` 있음 | 200 | S10 |
| D-3 | `/docs` | 301, `Location: /docs/` | S9 |
| D-4 | `/img/` + index.html 없음 | 403 | S10 |
| D-5 | `/img` + index.html 없음 | 301 (슬래시 검사가 먼저) | S9 |
| D-6 | `/docs/` + `docs/index.html` 권한 000 | 403 | S7, S10 |

### N. 메서드
| # | 입력 | 기대 | 결정 |
|---|---|---|---|
| N-1 | POST `/b.html` (있는 파일) | 405, `Allow: GET` | S8 |
| N-2 | POST `/없는파일` | Q-A가 정해지면 채움 | S8 |

### M. MIME
| # | 입력 | 기대 | 결정 |
|---|---|---|---|
| M-1 | 4번 표의 확장자 전부 | 표의 타입 그대로 | S14 |
| M-2 | `a.JPG`, `a.Html` | `image/jpeg`, `text/html; charset=utf-8` | S15 |
| M-3 | `a.hwp` | `application/octet-stream` | S17 |
| M-4 | `README` (확장자 없음) | `application/octet-stream` | S17 |
| M-5 | `a.tar.gz` | `application/octet-stream` (마지막 `.` 뒤 `gz`) | S15, S17 |
| M-6 | `/v1.2/readme` | `application/octet-stream` (디렉토리 이름의 `.`은 안 봄) | S15 |
| M-7 | `json`, `png` | charset 없음 | S16 |

### R. 응답 헤더
| # | 상황 | 기대 | 결정 |
|---|---|---|---|
| R-1 | 200, 파일 1234바이트 | `HTTP/1.1 200 OK`, `Content-Type`, `Content-Length: 1234`, `Connection`, `Date`, 끝이 `\r\n\r\n` | S12 |
| R-2 | keep_alive true / false | `Connection: keep-alive` / `Connection: close` | S12 |
| R-3 | 405 | `Allow: GET` 포함 | S8, S12 |
| R-4 | 301 | `Location: /docs/` 포함 | S9, S12 |
| R-5 | 고정 시각 784111777 (1994-11-06 08:49:37 UTC) | `Date: Sun, 06 Nov 1994 08:49:37 GMT` | S12 |
| R-6 | 아무 응답 | `Server` 헤더 없음 | S12 |

### E. 오류 응답 본문
| # | 상황 | 기대 | 결정 |
|---|---|---|---|
| E-1 | 404 | 본문 고정 HTML, `Content-Type: text/html; charset=utf-8`, `Content-Length` = 본문 길이 | S13 |
| E-2 | 5번의 비-200 코드 전부 | 상태 줄 사유 문구 정확, 본문 있음 | S13 |
| E-3 | 301 | 본문에 `/docs/` 링크 | S13 |

### L. 수명과 자원
| # | 확인 | 기대 | 결정 |
|---|---|---|---|
| L-1 | 200 결과를 정리 | fd 닫힘 (`fcntl(fd, F_GETFD)` 실패) | 수명 규칙 |
| L-2 | 오류로 끝나는 모든 경우 (403, 404, 301, 405) | 열린 fd 개수가 시작 전과 같음 | 수명 규칙 |
| L-3 | 위 테스트 전체 | ASan에서 누수·잘못된 접근 0 | Q5 |

### 다음 단계로 넘김
- 3단계: 파일을 8KB 조각으로 읽어 보내기, 보낸 위치를 진행 카드에 기록, **약속한 `Content-Length`보다 더 보내지 않고 파일이 먼저 끝나면 연결을 닫는다**, `sendfile`은 측정 후
- 4단계: 라우팅을 원문 경로로 볼지 푼 경로로 볼지, 데모 API 경로(REST)와 B5(GET·POST만) 관계, `X-Forwarded-For`, hop-by-hop 헤더 정리

## 9. 정한 세부 결정 (S, 2026-11-24)

| # | 결정 | 근거 |
|---|---|---|
| S1 | 정적 요청의 `?` 뒤(쿼리)는 잘라서 버린다 | RFC에 규정 없음 (서버 재량) |
| S2 | `%` 인코딩은 푼다. 딱 한 번만, 16진수 대소문자 모두 받음. `+`는 그대로 `+`. 루트 밖 검사는 푼 다음에 | 공백·한글 파일명 지원 |
| S3 | 깨진 인코딩(`%` 뒤 16진수 두 자리가 아님)은 400 | RFC 9112 §3 (잘못된 request-line은 400 SHOULD) |
| S4 | `%00`은 400 | RFC 규정 없음. C `open()`이 `\0`에서 끊겨 `.html` 검사를 우회하는 것을 막음 |
| S5 | `%2F`(인코딩된 `/`)는 404 | RFC 3986 §2.2 (예약 문자의 인코딩과 원래 글자는 다른 주소) |
| S6 | 루트 밖 차단: 푼 경로의 `.`·`..`를 글자로 정리하다 루트 위로 올라가면 403, 그다음 `realpath` 결과가 루트(경계 `/` 포함) 밖이면 403 | R4c (루트 밖 → 403). RFC 3986 §5.2.4 방식(루트 위 `..`를 버림)은 R4c와 맞지 않아 쓰지 않음 |
| S7 | 오류 매핑: 없음(ENOENT, ENOTDIR) 404, 권한 없음(EACCES) 403, 일반 파일·디렉토리가 아님 403, 그 밖 500 | — |
| S8 | 정적 경로의 POST는 405 + `Allow: GET` | RFC 9110 §9.1 (아는 메서드지만 대상이 허용 안 함 → 405), §15.5.6 (405에 Allow MUST) |
| S9 | `/`로 안 끝나는 디렉토리 요청은 301, `Location: 경로/` | 상대 링크가 맞게 풀리도록. Apache·nginx 기본 동작 |
| S10 | 디렉토리의 대표 파일은 `index.html` 하나. 없으면 403. 디렉토리 목록은 비목표 | nginx 기본 동작 |
| S11 | 2단계는 `open` + `fstat`까지 (fd와 크기). 읽어서 보내기는 3단계에서 조각씩 | 연결당 메모리 고정 (메모리는 유한) |
| S12 | 응답 헤더: 상태 줄, `Content-Type`, `Content-Length`, `Connection`, `Date`(IMF-fixdate, `gmtime_r`) + 405 `Allow`, 301 `Location`. `Server`는 넣지 않음 | RFC 9110 §6.6.1 (2xx·3xx·4xx에 Date MUST) |
| S13 | 200이 아닌 응답(301 포함)의 본문은 코드별 고정 짧은 HTML, `text/html; charset=utf-8`. 301은 이동 주소 링크 포함 | — |
| S14 | MIME 표는 4번 표 | — |
| S15 | 확장자는 마지막 경로 조각의 마지막 `.` 뒤, 대소문자 무시 | — |
| S16 | text 계열(html, css, js, txt)에 `; charset=utf-8`. json·이미지 등은 없음. 정적 파일은 UTF-8로 둔다는 전제 | — |
| S17 | 표에 없는 확장자와 확장자 없는 파일은 `application/octet-stream` | nginx 기본값 |
| S18 | 테스트 환경: Docker를 일반 사용자로 실행(Dockerfile 수정), 테스트 파일은 실행 중 `/tmp`에 만들고 지움 | root면 권한 000도 열려 403 검증 불가. git은 000 권한을 못 담고 윈도우에서 심볼릭 링크가 깨짐 |
