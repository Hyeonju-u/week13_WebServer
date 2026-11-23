# 1단계: HTTP 요청 헤더 파서 + 본문 모으기

> 원본: Notion "웹 서버 요구분석" 5번 섹션 (2026-11-23 기준)
> 관련 R: R1b, R1c, R2a, R3c, R3d(경계), R4a, R4b
> 완료 기준: 아래 "테스트 목록"의 테스트가 전부 통과. 부하 테스트는 7단계에서.

## 1. 범위

- **한다:** 헤더 끝 찾기, 헤더 복사, 헤더 파싱, 헤더 줄 나누기, 본문 모으기, 요청 해제
- **안 한다:** 소켓 읽기·쓰기, 응답 생성, 라우팅, 정적 파일, 프록시 전달 (다음 단계)
- 이 단계의 코드는 **소켓을 모른다.** 버퍼(바이트 배열)만 받는다. `sys/socket.h` 등을 쓰지 않는다.

## 2. 흐름 (헤더)

```
0. 연결 버퍼 8KB (연결마다 하나, 크기 고정, 절대 늘리지 않음)
1. (연결 관리) 읽은 바이트를 연결 버퍼 뒤에 이어 붙임 (남은 자리만큼만)
2. 맨 앞의 \r\n을 반복해서 걷어 냄 (P3). 맨 앞이 \r 없는 \n이면 BAD 400 (B29)
3. find_header_end → FOUND / NEED_MORE / BAD
4. FOUND면 헤더 영역(header_length 바이트)을 요청 전용 공간에 복사 (P13, P14)
5. parse_request_head를 "복사본"에 대고 실행 → request의 포인터는 처음부터 복사본 안을 가리킴 (P14)
6. 연결 버퍼에서 header_length만큼 지우고 남은 바이트를 앞으로 당김 (바로 해도 됨)
7. 본문 모으기 (아래 4번)
8. 요청이 끝나면 destroy_request가 헤더 복사본과 본문 버퍼를 함께 해제
```

## 3. 함수 (정해진 것)

```c
/* 헤더 끝 찾기: 해석 없이 빈 줄(\r\n\r\n) 위치만 찾는다 */
/* BAD이면 *status에 400 / 414 / 431 (P16) */
find_result find_header_end (const char * buf, size_t length, size_t * header_length, int * status);

/* 헤더 파싱: 복사본(헤더 전체)에 대고 실행. 소켓도 본문도 모른다 */
parse_result parse_request_head (const char * head, size_t head_length, request_t * request);

/* 헤더 영역에서 다음 줄을 꺼낸다. 파서와 (4단계) 프록시가 같이 쓴다 */
/* 결과: LINE_OK / LINE_END / LINE_BAD (P15) */
line_result next_header_line (const char * block, size_t length, size_t * position, header_view_t * line);
```

| 함수 → 결과 | 뜻 |
|---|---|
| `find_header_end` → `FOUND` | `header_length` = 헤더 영역 바이트 수 (빈 줄 포함) |
| `find_header_end` → `NEED_MORE` | 아직 빈 줄이 없고 8KB 안쪽 |
| `find_header_end` → `BAD` | 8KB 안에 첫 줄 끝(`\r\n`)이 없으면 414, 빈 줄이 없으면 431, `\r` 없는 `\n`이면 400 |
| `parse_request_head` → `OK` | 파싱 성공. `request`의 포인터는 복사본 안, `content_length` 채워짐 |
| `parse_request_head` → `BAD` | `request->status`에 응답 코드 (400, 411, 413, 501, 505). 이때 `status` 외 필드는 쓰지 않는다 |

**정해지지 않았던 것**(결과 enum, `request_t`·`header_view_t` 필드, 본문 모으기·헤더 복사 함수, 파일 구조, 빌드 도구)은 아래 8번 "정한 세부 결정"에 정리했다. 결과 이름은 접두어를 붙인다 (`FIND_BAD`, `PARSE_BAD`, P19).

## 4. 본문 모으기 (P11 ③)

- 본문은 연결 버퍼에 담지 않는다. 요청마다 **Content-Length만큼만** 따로 할당한 본문 버퍼에 모은다.
- 헤더와 같이 들어온 본문 앞부분은 연결 버퍼에서 본문 버퍼로 옮긴다 (Content-Length를 넘는 바이트는 옮기지 않고 연결 버퍼에 남긴다 = 다음 요청).
- 이후 읽기는 본문 버퍼로 **남은 만큼만** 한다.
- Content-Length가 0이거나 없으면 본문 버퍼를 만들지 않는다.
- GET에 본문이 있으면(P10) 할당 없이 읽어서 버리고, 다음 요청 경계는 지킨다.
- 할당 실패는 500 (연결 관리 단계에서 응답).

## 5. 파싱 검사 순서 (= 에러 우선순위, B32)

먼저 걸린 에러에서 바로 반환한다 (early return).

| 순서 | 검사 | 실패 |
|---|---|---|
| (find) | `\r` 없는 `\n` → 첫 줄 길이 → 헤더 전체 길이 | 400 → 414 → 431 |
| 1 | 요청 라인: 공백 하나로 나뉜 정확히 세 칸 (P2) | 400 |
| 2 | 메서드: `GET` / `POST`, 대소문자 구분 (B5, P1) | 501 |
| 3 | 경로: `/`로 시작하거나 `http://호스트[:포트]/경로` 절대 형식 (P4). 절대 형식이면 호스트를 떼어 내고 경로만 남기며, 호스트는 Host 헤더보다 우선 | 400 |
| 4 | 버전: `HTTP/1.1` (B1) | 505 |
| 5 | 헤더 줄 형식 (`next_header_line`): `:` 없음, 이름과 `:` 사이 공백, 공백으로 시작하는 줄(줄 접기) (P5) | 400 |
| 6 | Host: 정확히 하나 (B2), 값이 비어 있으면 안 됨 (P32). 절대 주소 형식이어도 필요 (RFC 9112 3.2) | 400 |
| 7 | Transfer-Encoding: 있으면 (B3). Content-Length 검사보다 먼저 (P18) | 501 |
| 8 | Content-Length: 중복 없음 (B30), 숫자만 (P9, P31), 1MB 이하 (B4), 읽는 도중 상한을 넘으면 멈춤(넘침 방지). POST인데 없으면 411 (B3) | 400 / 413 / 411 |
| 9 | Connection: 쉼표로 나눈 값 중 `close`(대소문자 무시)가 있으면 연결 유지 안 함, 없으면 유지 (P8). Connection 줄이 여러 개면 모두 본다 (P33) | — |

기타
- 헤더 값 앞뒤 공백은 잘라낸다 (P6)
- 헤더 이름은 대소문자를 구분하지 않는다
- 헤더 개수 상한은 없다 (헤더 전체 8KB가 사실상 상한, P7)
- 경로는 원문 그대로 담는다. `?` 자르기, `%` 인코딩 풀기는 2단계 (P12)
- 그 밖의 헤더는 따로 저장하지 않는다. 헤더 복사본 전체를 요청이 들고 있고, 필요하면 `next_header_line`으로 다시 훑는다 (P13)

## 6. 수명 규칙

- 헤더 복사본: `find_header_end`가 `FOUND`일 때 만들고, 그 요청이 끝날 때 해제
- 본문 버퍼: 본문이 있을 때 만들고, 그 요청이 끝날 때 해제
- 해제는 `destroy_request` 하나로. 해제 후 포인터를 `NULL`로 만들어 두 번 불려도 안전하게
- 한 연결이 동시에 들고 있는 요청은 최대 하나

## 7. 테스트 목록

- 테스트 이름이나 주석에 아래 ID를 적는다 (예: `/* F5 (B4) */`)
- 표에 없는 케이스는 **추가하지 말고 제안만** 한다
- 경계: 8KB = 8,192바이트, 1MB = 1,048,576바이트까지 허용, 넘으면 거절

### F. find_header_end
| # | 입력 | 기대 | 결정 |
|---|---|---|---|
| F1 | 헤더 + 빈 줄 | FOUND, header_length 정확 | R1c |
| F2 | 빈 줄이 아직 없음 (8KB 미만) | NEED_MORE | R1c |
| F3 | GET 요청 두 개가 붙어 옴 | FOUND, 첫 요청 헤더 길이 | R3d |
| F4 | 요청을 한 바이트씩 늘려 가며 넣음 | 마지막에만 FOUND, 그 전엔 NEED_MORE | R3c |
| F5 | 첫 줄 끝이 8KB 안에 없음 | BAD 414 | B4 |
| F6 | 빈 줄이 8KB 안에 없음 | BAD 431 | B4 |
| F7 | `\r` 없는 `\n`이 중간에 있음 | BAD 400 | B29 |
| F8 | `\r\n\r\n`으로 시작하는 요청 | 앞 빈 줄을 걷어 낸 뒤 진짜 헤더로 FOUND | P3 |
| F9 | 맨 앞이 `\n` | BAD 400 | B29, P3 |
| F10 | 헤더 정확히 8,192바이트 / 8,193바이트 | FOUND / BAD 431 | B4 경계 |
| F11 | `\n`이 따라오지 않는 `\r` (`Host: a\rb`) | BAD 400 | P30 |
| F12 | 버퍼가 `\r`로 끝남 | NEED_MORE | P29 |

### H. next_header_line
| # | 입력 | 기대 | 결정 |
|---|---|---|---|
| H1 | 헤더 세 줄 | 세 번 꺼내고 네 번째에 끝 | P13 |
| H2 | `Content-Length:   16  ` | 값 `16` | P6 |
| H3 | `Host : x` | 오류 | P5 |
| H4 | 공백으로 시작하는 줄 | 오류 | P5 |
| H5 | `:` 없는 줄 | 오류 | P5 |

### P. parse_request_head
| # | 입력 | 기대 | 결정 |
|---|---|---|---|
| P-1 | GET + Host | OK, 메서드·경로·버전 정확 | R1b |
| P-2 | POST + `Content-Length: 16` | OK, content_length 16 | R2a |
| P-3 | `content-length` (소문자 이름) | 같게 인식 | R1c |
| P-4 | `Connection: close` / 없음 | 유지 안 함 / 유지 | P8 |
| P-5 | `Connection: Keep-Alive, CLOSE` | 유지 안 함 | P8 |
| P-6 | `GET http://localhost:8080/a HTTP/1.1` | OK, 경로 `/a` | P4 |
| P-7 | 절대 주소 형식 + Host 헤더가 다름 | 요청 라인 쪽 호스트 사용 | P4 |
| P-8 | 경로 `/a%20b.html?x=1` | 원문 그대로 | P12 |
| P-9 | GET + `Content-Length: 5` | OK, content_length 5 | P10 |
| P-10 | 짧은 헤더 500개 (8KB 이내) | OK | P7 |
| P-11 | Content-Length 정확히 1,048,576 | OK | B4 경계 |
| P-12 | 성공 후 헤더 영역을 `next_header_line`으로 훑기 | Content-Type 등 원래 헤더가 전부 나옴 | P13 |
| P-13 | 요청 라인 `HELLO` | 400 | P2 |
| P-14 | 공백 두 칸 `GET  /a HTTP/1.1` | 400 | P2 |
| P-15 | 칸 네 개 `GET /a b HTTP/1.1` | 400 | P2 |
| P-16 | 경로가 `/`로 시작 안 하고 절대 형식도 아님 | 400 | P4 |
| P-17 | `PUT`, `HEAD` | 501 | B5 |
| P-18 | 소문자 `get` | 501 | P1 |
| P-19 | `HTTP/1.0`, `HTTP/9.9` | 505 | B1 |
| P-20 | `:` 없는 헤더 줄 / 이름 뒤 공백 | 400 | P5 |
| P-21 | Host 없음 / Host 두 개 | 400 | B2 |
| P-22 | POST인데 Content-Length 없음 | 411 | B3 |
| P-23 | Content-Length `abc`, `-1`, `+5`, 빈 값 | 400 | B3, P9 |
| P-24 | Content-Length `99999999999999999999` | 413 (넘쳐서 작은 값이 되지 않음) | P9 |
| P-25 | Content-Length 1,048,577 | 413 | B4 경계 |
| P-26 | Content-Length 두 개 (같은 값 / 다른 값) | 400 | B30 |
| P-27 | `Transfer-Encoding: chunked` | 501 | B3 |
| P-28 | `HTTP/1.0` + Content-Length `abc` | 505 (버전이 먼저) | B32 |
| P-29 | `PUT` + Host 없음 | 501 (메서드가 먼저) | B32 |
| P-30 | POST + `Transfer-Encoding: chunked` + Content-Length 없음 | 501 | P18 |
| P-31 | 빈 Host 값 (`Host:`) | 400 | P32 |
| P-32 | `Connection: keep-alive` 줄과 `Connection: close` 줄 | 유지 안 함 | P33 |
| P-33 | Content-Length `99999abc` / `007` | 400 / OK, content_length 7 | P31 |

### B. 본문 모으기
| # | 입력 | 기대 | 결정 |
|---|---|---|---|
| B-1 | 헤더 뒤에 본문 16바이트가 한 번에 다 옴 | 16바이트 복사, 바로 완성 | P11 |
| B-2 | 헤더 뒤에 본문 5바이트만 옴 | 5바이트 복사, 미완성 (11바이트 남음) | P11 |
| B-3 | 본문 16바이트 뒤에 다음 요청 앞부분이 붙어 옴 | 본문엔 16바이트만, 나머지는 연결 버퍼에 남김 | P11 |
| B-4 | Content-Length 0 또는 없음 | 본문 버퍼를 만들지 않음 | P11 |
| B-5 | GET + 본문 5바이트 + 다음 요청 | 5바이트 버리고 다음 요청 경계 유지 | P10 |
| B-6 | 5바이트를 받은 뒤 남은 11바이트를 본문 버퍼에 직접 쓰고 `add_body_bytes(11)` | 완성, 본문 16바이트 정확 | P26 |

### L. 수명과 메모리
| # | 확인 | 기대 | 결정 |
|---|---|---|---|
| L-1 | 헤더 복사 후 원래 연결 버퍼를 덮어씀 | request의 경로·Host가 그대로 | P14 |
| L-2 | `destroy_request` 두 번 호출 | 안전 (두 번 해제 안 됨) | 수명 규칙 |
| L-3 | 정상 / BAD / 중간 끊김 경로 | ASan(`-fsanitize=address,leak`)으로 누수·잘못된 접근 0 | Q5 |

### 다음 단계로 넘김
- X-1: BAD 응답에 `Connection: close` + 연결 닫힘 (B31) → 응답·연결 관리 단계
- X-2: 설정 파일로 상한 값 변경 반영 (B4, D10) → 설정 단계

## 8. 정한 세부 결정 (P15~, 2026-11-23)

| # | 결정 |
|---|---|
| P15 | `next_header_line`의 결과는 `line_result` 3값: `LINE_OK`(한 줄 꺼냄) / `LINE_END`(영역 끝) / `LINE_BAD`(줄 형식 오류) |
| P16 | `find_header_end`는 출력 인자 `int * status`를 더 받아, `FIND_BAD`일 때 400 / 414 / 431을 적는다 |
| P17 | 맨 앞 `\r\n` 걷어 내기는 별도 함수 `skip_leading_empty_lines (char * buffer, size_t * length)`. 연결 버퍼를 앞으로 당기고, 맨 앞이 `\r` 없는 `\n`이면 `false`(400). 그다음 `find_header_end`를 부른다 |
| P18 | Transfer-Encoding 검사를 Content-Length 검사보다 먼저 한다. POST + TE + Content-Length 없음 → 501 |
| P19 | 결과 enum에 함수 접두어: `FIND_FOUND`·`FIND_NEED_MORE`·`FIND_BAD`, `PARSE_OK`·`PARSE_BAD`, `LINE_*`, `BODY_*`. 타입 이름은 명세대로 `find_result`, `parse_result` |
| P20 | `header_view_t` = `name`, `name_length`, `value`, `value_length`. 복사본 안을 가리키는 시작 + 길이 (`\0` 없음) |
| P21 | `request_t` 필드: 소유 `head`·`head_length`, `body`·`body_received` / view `path`, `host`, `version`, `headers` (각각 + `_length`) / `method`(`METHOD_GET`·`METHOD_POST`), `content_length`, `keep_alive`, `status`, `discard_remaining` |
| P22 | `request_t.headers`는 요청 라인 다음부터 마지막 빈 줄 앞까지. `next_header_line`에는 이 범위만 넘긴다 |
| P23 | 호스트는 포트까지 담는다 (`localhost:8080`). 절대 주소 형식이면 요청 라인 쪽 값을 쓴다 (P4) |
| P24 | `request_t`는 연결 구조체에 박아 두는 값 타입. 0으로 채워 시작하고, `destroy_request (request_t *)`가 `head`·`body`를 free한 뒤 `NULL`로 만든다 |
| P25 | 헤더 복사는 `bool copy_request_head (request_t * request, const char * buffer, size_t header_length)`. 실패(`false`, → 500)하면 `request`는 그대로 |
| P26 | 본문 모으기는 두 함수. `start_body (request_t *, char * buffer, size_t * buffer_length)`: 본문 버퍼 할당(POST) 또는 버리기(GET, P10), 연결 버퍼의 앞부분을 옮기거나 버리고 당긴다. `add_body_bytes (request_t *, size_t count)`: 소켓이 직접 읽은 만큼 늘린다. 결과는 `BODY_DONE` / `BODY_NEED_MORE` / `BODY_NO_MEMORY`(→ 500) |
| P27 | 연결 버퍼 앞부분 지우기 도우미 `remove_buffer_prefix (char * buffer, size_t * length, size_t count)` |
| P28 | 상한은 상수 `MAX_HEADER_SIZE 8192`, `MAX_BODY_SIZE 1048576` (설정 연동은 X-2) |
| P29 | 8KB 세기: 헤더 길이는 마지막 `\r\n\r\n` 포함, 첫 줄 길이는 `\r\n` 포함, 앞 빈 줄은 걷어 낸 뒤 센다. 버퍼가 `\r`로 끝나면 NEED_MORE |
| P30 | `\n`이 따라오지 않는 홀로 있는 `\r`은 400 (B29와 짝) |
| P31 | Content-Length는 먼저 전부 숫자인지 확인(아니면 400)한 뒤, 상한을 둔 누적(넘으면 413). 앞자리 0은 허용 (`007` = 7) |
| P32 | Host 값이 비어 있으면 400 |
| P33 | Connection 줄이 여러 개면 모두 보고, 어디든 `close`가 있으면 연결 유지 안 함 |
| P34 | 파일: `include/http_request.h`, `src/http_request.c`, `tests/check.h`, `tests/test_http_request.c`, `Makefile` |
| P35 | 검사 매크로 (B6): `BEGIN_TEST(id)`, `CHECK(조건)`, `CHECK_INT(실제, 기대)`, `CHECK_VIEW(시작, 길이, "기대")`. 실패하면 `[ID] 파일:줄`을 찍고 계속, 끝에 통과/실패 개수, 실패가 있으면 종료 코드 1 |
| P36 | L-3의 "중간 끊김"은 본문 일부만 받은 상태에서 `destroy_request`를 부르는 것으로 표현한다 |
