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
| 2 | `%` 풀기: 앞에서부터 보고 처음 걸린 것. 깨진 인코딩 / `%00` / `%2F` / 날것의 제어 바이트(0x00~0x1F, 0x7F) | 400 / 400 / 404 / 400 | S3, S4, S5, S24 |
| 3 | `.`·`..` 정리 중 루트 위로 올라감 | 403 | S6 |
| 3-1 | 정적 경로에 POST (파일시스템을 보기 전. `POST /없는파일`도 405) | 405 | S8, S20 |
| 4 | 루트와 붙인 경로 확인: 없음 / 권한 없음 / 이름이 너무 김(`ENAMETOOLONG`) / 링크 고리(`ELOOP`) / 그 밖 | 404 / 403 / 404 / 403 / 500 | S7, S21 |
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
- 항상: `Content-Type`, `Content-Length`, `Connection`(`keep-alive` / `close`)
- 5xx가 아니면: `Date`(IMF-fixdate, 예 `Sun, 06 Nov 1994 08:49:37 GMT`). 5xx에는 넣지 않는다 (S23)
- 405에 `Allow: GET`, 301에 `Location: <손님이 보낸 원문 경로, 쿼리 뺌>/` (S22)
- `Server` 헤더는 넣지 않는다
- 200이 아닌 응답(301 포함)은 본문이 코드별 고정 짧은 HTML (`text/html; charset=utf-8`). 301 본문에도 주소를 넣지 않는다 (S19)
- 오류 본문은 헤더 바로 뒤, 같은 버퍼에 이어 쓴다 (S27)
- 다루는 코드와 사유 문구: 200 OK, 301 Moved Permanently, 400 Bad Request, 403 Forbidden, 404 Not Found, 405 Method Not Allowed, 411 Length Required, 413 Content Too Large, 414 URI Too Long, 431 Request Header Fields Too Large, 500 Internal Server Error, 501 Not Implemented, 505 HTTP Version Not Supported

## 6. 수명 규칙

- ⑥에서 연 파일 fd는 결과를 받은 쪽이 소유한다. 3단계에서는 연결의 진행 카드가 들고 있다가 응답이 끝나면 닫는다.
- 오류로 끝나는 모든 경로에서 연 fd는 닫힌다 (L-2).
- 정리 함수는 두 번 불려도 안전하게.

## 7. 정한 질문과 인터페이스 (2026-11-24)

### 질문 → 결정
| # | 질문 | 결정 |
|---|---|---|
| Q-A | 정적 POST 405의 검사 위치 | S20 |
| Q-B | `ENAMETOOLONG`, `ELOOP`의 코드 | S21 |
| Q-C | 301 `Location`에 쓸 경로 | S22 |
| Q-D | `Date`를 5xx에도 넣을지 | S23 |

### 인터페이스 (S25~S28)

```c
/* include/http_response.h */
#define MAX_RESPONSE_HEAD_SIZE (MAX_HEADER_SIZE + 512)  /* S27 */

typedef struct {
    int status;
    bool keep_alive;
    const char * content_type;   /* 200일 때만 쓴다 (MIME 표 문자열, 빌림) */
    size_t content_length;       /* 200일 때만 쓴다 (파일 크기) */
    const char * location;       /* 301일 때만. request->head 안의 원문 경로 (빌림) */
    size_t location_length;      /* 쿼리 앞까지. 끝의 '/'는 쓸 때 붙인다 */
} response_head_t;

/* 반환: buffer에 쓴 바이트 수 (헤더 + 오류 본문) */
size_t write_response_head (const response_head_t * head, time_t now,
                            char * buffer);

/* include/static_file.h */
typedef struct {
    response_head_t head;
    int fd;                      /* 200일 때만 열림. 그 밖 -1 */
} static_result_t;

char * copy_static_root (const char * path);   /* NULL = realpath 실패. free */
void open_static_file (const request_t * request, const char * root,
                       static_result_t * result);
const char * find_mime_type (const char * path, size_t length);
void close_static_file (static_result_t * result);  /* 두 번 불러도 안전 */
```

- 파서 오류(411, 431 등)와 4단계 502/504도 `response_head_t`를 채워 `write_response_head`를 부른다.

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
| N-2 | POST `/없는파일` | 405, `Allow: GET` (파일 확인보다 먼저) | S8, S20 |

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
| E-3 | 301 | 본문이 고정 HTML이고 경로 글자(`/docs/`)가 없음, `Location: /docs/`는 있음 | S13, S19 |

### L. 수명과 자원
| # | 확인 | 기대 | 결정 |
|---|---|---|---|
| L-1 | 200 결과를 정리 | fd 닫힘 (`fcntl(fd, F_GETFD)` 실패) | 수명 규칙 |
| L-2 | 오류로 끝나는 모든 경우 (403, 404, 301, 405) | 열린 fd 개수가 시작 전과 같음 | 수명 규칙 |
| L-3 | 위 테스트 전체 | ASan에서 누수·잘못된 접근 0 | Q5 |

### 다음 단계로 넘김
- 3단계: 파일을 8KB 조각으로 읽어 보내기, 보낸 위치를 진행 카드에 기록, **약속한 `Content-Length`보다 더 보내지 않고 파일이 먼저 끝나면 연결을 닫는다**, `sendfile`은 측정 후, 정적 단계의 400(깨진 인코딩 등)에도 B31(`Connection: close`)을 적용할지 (2단계는 요청의 keep_alive를 그대로 따름)
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
| S12 | 응답 헤더: 상태 줄, `Content-Type`, `Content-Length`, `Connection`, `Date`(IMF-fixdate, `gmtime_r`, 5xx 제외 S23) + 405 `Allow`, 301 `Location`. `Server`는 넣지 않음 | RFC 9110 §6.6.1 (2xx·3xx·4xx에 Date MUST) |
| S13 | 200이 아닌 응답(301 포함)의 본문은 코드별 고정 짧은 HTML, `text/html; charset=utf-8`. 301도 고정 문구만 (S19) | — |
| S14 | MIME 표는 4번 표 | — |
| S15 | 확장자는 마지막 경로 조각의 마지막 `.` 뒤, 대소문자 무시 | — |
| S16 | text 계열(html, css, js, txt)에 `; charset=utf-8`. json·이미지 등은 없음. 정적 파일은 UTF-8로 둔다는 전제 | — |
| S17 | 표에 없는 확장자와 확장자 없는 파일은 `application/octet-stream` | nginx 기본값 |
| S18 | 테스트 환경: Docker를 일반 사용자로 실행(Dockerfile 수정), 테스트 파일은 실행 중 `/tmp`에 만들고 지움 | root면 권한 000도 열려 403 검증 불가. git은 000 권한을 못 담고 윈도우에서 심볼릭 링크가 깨짐 |
| S19 | 301 본문에 이동 주소를 넣지 않는다. 이동은 `Location` 헤더가 맡는다 | 손님이 보낸 경로 글자를 HTML에 넣지 않기 위해 (XSS) |
| S20 | 정적 POST 405는 `.`·`..` 정리(3번) 바로 뒤, 파일시스템을 보기 전에 검사한다. `POST /없는파일`도 405 | 정적 영역 전체가 POST를 받지 않으므로 파일 유무와 무관. 루트 밖 403은 여전히 먼저 |
| S21 | `ENAMETOOLONG`은 404, `ELOOP`은 403 | 너무 긴 이름은 있을 수 없는 파일, 고리 링크는 따라갈 수 없는 링크. 손님 잘못을 500으로 남기지 않음 |
| S22 | 301 `Location`은 손님이 보낸 원문 경로(인코딩 그대로, 쿼리 뺌) + `/` | 다시 인코딩할 규칙이 필요 없음. 쿼리는 S1대로 버림 |
| S23 | 5xx 응답에는 `Date`를 넣지 않는다 | RFC 9110 §6.6.1 (5xx는 MAY) |
| S24 | 경로에 날것의 제어 바이트(0x00~0x1F, 0x7F)가 있으면 400. 검사 순서 2번에서 `%` 풀기와 같이 본다 | RFC 3986에 없는 글자 (S3과 같은 근거). 날것의 `\0`이 S4를 우회하는 것을 막음 |
| S25 | 공개 함수: `copy_static_root`, `open_static_file`, `find_mime_type`, `close_static_file`, `write_response_head`. 결과는 `static_result_t` = `response_head_t` + `fd` (7번 인터페이스) | 경로 검사와 헤더 쓰기를 따로 테스트. 헤더 쓰기는 파서 오류·4단계에서도 씀 |
| S26 | 정적 루트는 시작할 때 `copy_static_root`로 한 번 `realpath`하고 매 요청에 빌려 넘긴다. 루트 `/`도 받는다 | 요청마다 realpath 안 함. 경계 비교 기준이 실행 중 바뀌지 않음 |
| S27 | 응답 헤더 버퍼는 고정 배열 `MAX_RESPONSE_HEAD_SIZE` = `MAX_HEADER_SIZE` + 여유. 여유는 301에서 경로를 뺀 나머지 290바이트(상태 줄 32, Content-Type 40, Content-Length 21, Connection 24, Date 37, `Location: /` 13, 빈 줄 2, 본문 121)보다 큰 가장 가까운 2의 거듭제곱 512. 넘침 처리 코드는 없음. 오류 본문은 헤더 바로 뒤에 이어 씀 | 경로는 입력에서 이미 8KB 안으로 막힘. 손님 글자가 들어가는 곳은 301 Location 한 줄뿐 (S19) |
| S28 | `write_response_head`는 현재 시각을 `time_t now` 인자로 받는다 | 테스트에서 고정 시각(R-5)을 넣기 위해 |
| S29 | 파일: `include/static_file.h`, `src/static_file.c`, `include/http_response.h`, `src/http_response.c`, `tests/test_static_file.c` | 응답 쓰기를 정적 파일과 분리 |
| S30 | Makefile: `make test`·`make asan`이 1단계·2단계 실행 파일을 차례로 돌리고 실패하면 멈춘다. `CFLAGS`에 `-D_XOPEN_SOURCE=700` | `check.h` 카운터가 static이라 테스트 파일마다 실행 파일 하나. `realpath`·`gmtime_r`·`mkfifo` 선언. glibc는 `_POSIX_C_SOURCE`만으로는 `realpath`를 선언하지 않음 |
| S31 | Dockerfile: `USER ubuntu` (ubuntu:24.04에 이미 있는 uid 1000) | S18 (root가 아니어야 O-4·D-6 검증). uid 1000이 이미 있어 `useradd -u 1000`은 실패함 |
| S32 | 테스트 방법: `request_t`는 직접 채움, "파일을 찾음"은 `st_dev`·`st_ino` 비교, 임시 파일은 만든 목록을 거꾸로 지움, L-2의 fd 개수는 `/proc/self/fd` 항목 수 | 2단계만 떼어 봄. 추가 매크로 없음. D5 리눅스 전용 |
