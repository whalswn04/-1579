# 네트워크 프로그래밍 최종 프로젝트 보고서

## 1. 프로젝트명

TalkLab Messenger: WebSocket 기반 실시간 1:1 메신저

## 2. 개발 목적

본 프로젝트의 목적은 메신저 서비스의 핵심 네트워크 구조를 직접 구현하는 것이다. 단순 화면 구현이 아니라 HTTP REST API, WebSocket, 세션 인증, 데이터베이스 저장, 브라우저 클라이언트 연동을 하나의 동작 가능한 시스템으로 구성했다.

## 3. 구현 범위

구현한 기능은 다음과 같다.

- 회원가입과 로그인
- 비밀번호 해시 저장
- 세션 토큰 발급 및 Bearer 인증
- 친구 추가
- 1:1 채팅방 생성
- 메시지 DB 저장
- 메시지 이력 조회
- WebSocket 실시간 메시지 송수신
- PWA 및 Electron 실행 지원

## 4. 시스템 구조

```text
Browser / PWA / Electron
        │
        │ HTTP REST + WebSocket JSON
        ▼
C++17 Boost.Beast Server
        │
        │ Boost.JSON JSON parsing/serialization
        │
        │ SQLite C API
        ▼
SQLite Database
```

서버는 HTTP 요청과 WebSocket Upgrade 요청을 같은 TCP 포트에서 처리한다. `/api/*` 경로는 REST API로 처리하고, `/ws?token=...` 경로는 WebSocket 세션으로 업그레이드한다.

## 5. 네트워크 프로그래밍 요소

### 5-1. HTTP REST

회원가입, 로그인, 친구, 채팅방, 메시지 이력 조회는 HTTP 요청/응답 모델로 구현했다. 각 API는 JSON을 요청 본문 또는 응답 본문으로 사용한다.

### 5-2. WebSocket

채팅 메시지는 HTTP polling 대신 WebSocket으로 송수신한다. 클라이언트는 로그인 후 발급받은 토큰을 쿼리 문자열에 포함해 `/ws`에 연결한다. 서버는 토큰을 검증한 뒤 해당 사용자의 연결을 ChatHub에 등록한다.

### 5-3. 실시간 브로드캐스트

사용자가 WebSocket으로 메시지를 보내면 서버는 다음 순서로 처리한다.

1. JSON 메시지를 파싱한다.
2. 사용자가 해당 채팅방의 멤버인지 확인한다.
3. 메시지를 SQLite `messages` 테이블에 저장한다.
4. 채팅방의 `last_message`, `last_message_at`을 갱신한다.
5. `room_members`에서 대상 사용자 목록을 가져온다.
6. ChatHub에 등록된 각 사용자의 WebSocket 연결로 메시지를 브로드캐스트한다.

## 6. DB 설계

| 테이블 | 목적 |
|---|---|
| users | 사용자 계정 정보 저장 |
| sessions | 로그인 토큰과 사용자 매핑 |
| friends | 친구 관계 저장 |
| chat_rooms | 채팅방 메타데이터 저장 |
| room_members | 채팅방 참여자 저장 |
| messages | 메시지 본문과 발신자 저장 |

## 7. 보안 처리

비밀번호는 원문으로 저장하지 않고 PBKDF2-HMAC-SHA256으로 해시한다. 사용자마다 랜덤 salt를 생성하며, 로그인 검증 시 constant-time 비교 함수를 사용해 단순 문자열 비교보다 안전하게 처리했다. 세션 토큰도 OpenSSL `RAND_bytes`로 생성한다.

## 8. 실행 방법

```bash
./install_deps_ubuntu.sh
cmake -S . -B build
cmake --build build -j2
./build/talk_server 8080 web data/talklab.db
```

브라우저에서 `http://localhost:8080`에 접속한다.

## 9. 테스트 결과

테스트 절차는 다음과 같다.

1. `user01`, `user02` 두 계정을 생성한다.
2. 두 브라우저에서 각각 로그인한다.
3. `user01`이 `user02`를 친구로 추가한다.
4. 1:1 채팅방을 만든다.
5. `user01`이 메시지를 보내면 `user02` 화면에 즉시 표시된다.
6. 새로고침 후 메시지 이력이 유지된다.

## 10. 한계와 향후 개선

현재 구현은 수업 프로젝트 제출에 맞춘 단일 서버 구조다. 실제 서비스 수준으로 확장하려면 비동기 I/O 기반 세션 관리, HTTPS, 쿠키 기반 SameSite 세션, 메시지 읽음 처리, 파일 업로드, 푸시 알림, 서버 수평 확장 시 Redis Pub/Sub 같은 외부 브로커가 필요하다.
