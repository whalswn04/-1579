# Test Log

검증 일자: 2026-06-01

## 1. 정적 검사

```bash
node --check web/app.js
node --check desktop/main.js
```

결과: JavaScript 문법 오류 없음.

## 2. CMake 빌드

```bash
cmake -S . -B build
cmake --build build -j2
```

결과: `talk_server` 빌드 성공.

## 3. REST API 수동 테스트

테스트한 항목:

- `GET /api/health`
- `POST /api/register`
- `POST /api/login`
- `POST /api/friends`
- `GET /api/friends`
- `POST /api/rooms/direct`
- `POST /api/messages`
- `GET /api/rooms/{id}/messages`

결과: 회원가입, 로그인, 친구 추가, 채팅방 생성, 메시지 저장 및 조회 성공.

## 4. WebSocket 테스트

테스트 절차:

1. `user01`, `user02` 토큰으로 각각 WebSocket 연결.
2. 두 연결 모두 `welcome` 이벤트 수신 확인.
3. `user01`이 `{ "type": "chat", "room_id": 1, "text": "WS 테스트" }` 전송.
4. `user01`, `user02` 양쪽 모두 `message` 이벤트 수신 확인.

결과: 실시간 브로드캐스트 정상 동작.
