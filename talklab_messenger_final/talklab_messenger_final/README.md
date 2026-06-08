# TalkLab Messenger Final Project

네트워크 프로그래밍 수업 최종 프로젝트용 메신저입니다. 카카오톡을 그대로 복제하지 않고, 메신저의 핵심 네트워크 구조인 REST API, WebSocket 실시간 송수신, 세션 인증, SQLite 메시지 저장을 직접 구현한 교육용 프로젝트입니다.

## 1. 주요 기능

- 회원가입, 로그인, 로그아웃
- PBKDF2-SHA256 기반 비밀번호 해시 저장
- Bearer token 기반 세션 인증
- 친구 추가 및 친구 목록 조회
- 1:1 채팅방 생성
- 채팅방 목록, 최근 메시지, 메시지 이력 조회
- WebSocket 기반 실시간 메시지 브로드캐스트
- SQLite 기반 사용자, 세션, 친구, 채팅방, 메시지 저장
- 브라우저 PWA 설치 지원
- Electron 데스크톱 앱 껍데기 포함

## 2. 프로젝트 구조

```text
talklab_messenger_final/
├─ server/src/              # C++17 서버: HTTP REST + WebSocket + SQLite
│  ├─ main.cpp              # HTTP 라우터, WebSocket 세션, 브로드캐스트 허브
│  ├─ database.cpp          # SQLite 스키마와 CRUD
│  ├─ database.h
│  ├─ security.cpp          # 랜덤 토큰, PBKDF2, constant-time compare
│  └─ security.h
├─ web/                     # JavaScript SPA + PWA
│  ├─ index.html
│  ├─ app.js
│  ├─ style.css
│  ├─ manifest.json
│  ├─ sw.js
│  └─ assets/icon.svg
├─ desktop/                 # Electron 데스크톱 앱 실행용
├─ docs/PROJECT_REPORT.md   # 제출용 보고서 초안
├─ data/                    # SQLite DB 생성 위치
├─ CMakeLists.txt
├─ install_deps_ubuntu.sh
└─ .gitignore
```

## 3. 사용 기술

| 구분 | 기술 |
|---|---|
| 서버 | C++17, Boost.Asio, Boost.Beast, Boost.JSON |
| 실시간 통신 | WebSocket over TCP |
| REST API | HTTP/1.1 JSON API |
| DB | SQLite3 |
| 보안 | OpenSSL RAND_bytes, PBKDF2-HMAC-SHA256 |
| 프론트엔드 | HTML, CSS, Vanilla JavaScript |
| 앱 형태 | PWA, Electron |
| 빌드 | CMake |

## 4. Ubuntu / GitHub Codespaces 실행

Boost.JSON을 사용하므로 Boost 1.75 이상이 필요합니다. GitHub Codespaces 기본 이미지 또는 Ubuntu 24.04 계열에서는 `libboost-dev` 설치로 충족됩니다.

### 4-1. 의존성 설치

```bash
./install_deps_ubuntu.sh
```

직접 설치하는 경우:

```bash
sudo apt update
sudo apt install -y build-essential cmake libboost-dev libssl-dev libsqlite3-dev nodejs npm
```

### 4-2. C++ 서버 빌드

```bash
cmake -S . -B build
cmake --build build -j2
```

### 4-3. 서버 실행

```bash
./build/talk_server 8080 web data/talklab.db
```

실행 후 브라우저에서 다음 주소를 엽니다.

```text
http://localhost:8080
```

GitHub Codespaces에서는 `PORTS` 탭에서 8080 포트를 열고 Forwarded Address로 접속합니다.

## 5. 테스트 시나리오

1. 첫 번째 브라우저에서 계정 `user01`을 가입합니다.
2. 시크릿 창 또는 다른 브라우저에서 계정 `user02`를 가입합니다.
3. 각각 로그인합니다.
4. `user01` 화면에서 친구 아이디 입력란에 `user02`를 입력해 친구를 추가합니다.
5. 친구 목록에서 `채팅` 버튼을 눌러 1:1 채팅방을 생성합니다.
6. 메시지를 보내고, `user02` 화면에서 실시간으로 수신되는지 확인합니다.
7. 새로고침 후 이전 메시지가 DB에서 다시 로드되는지 확인합니다.

## 6. REST API 요약

### 상태 확인

```http
GET /api/health
```

### 회원가입

```http
POST /api/register
Content-Type: application/json

{
  "user_id": "user01",
  "display_name": "사용자1",
  "password": "123456"
}
```

### 로그인

```http
POST /api/login
Content-Type: application/json

{
  "user_id": "user01",
  "password": "123456"
}
```

응답의 `token`을 이후 요청의 Authorization 헤더에 넣습니다.

```http
Authorization: Bearer <token>
```

### 내 정보

```http
GET /api/me
Authorization: Bearer <token>
```

### 친구 목록

```http
GET /api/friends
Authorization: Bearer <token>
```

### 친구 추가

```http
POST /api/friends
Authorization: Bearer <token>
Content-Type: application/json

{
  "friend_user_id": "user02"
}
```

### 1:1 채팅방 생성

```http
POST /api/rooms/direct
Authorization: Bearer <token>
Content-Type: application/json

{
  "friend_user_id": "user02"
}
```

### 채팅방 목록

```http
GET /api/rooms
Authorization: Bearer <token>
```

### 메시지 이력

```http
GET /api/rooms/1/messages?limit=80
Authorization: Bearer <token>
```

### HTTP로 메시지 전송

```http
POST /api/messages
Authorization: Bearer <token>
Content-Type: application/json

{
  "room_id": 1,
  "text": "안녕하세요"
}
```

## 7. WebSocket 프로토콜

연결 주소:

```text
ws://localhost:8080/ws?token=<token>
```

### Ping

```json
{
  "type": "ping"
}
```

### 채팅 메시지 전송

```json
{
  "type": "chat",
  "room_id": 1,
  "text": "안녕하세요"
}
```

### 서버 브로드캐스트

```json
{
  "type": "message",
  "message": {
    "id": 1,
    "room_id": 1,
    "sender": {
      "id": 1,
      "user_id": "user01",
      "display_name": "사용자1"
    },
    "body": "안녕하세요",
    "created_at": "2026-06-01 04:00:00"
  },
  "time": "2026-06-01T04:00:00Z"
}
```

## 8. Electron 데스크톱 실행

서버를 먼저 실행한 뒤 별도 터미널에서 실행합니다.

```bash
cd desktop
npm install
npm start
```

기본적으로 Electron은 `http://localhost:8080`을 엽니다. 다른 주소를 열려면 다음처럼 실행합니다.

```bash
APP_URL=http://localhost:8080 npm start
```

## 9. GitHub 제출 방법

새 GitHub 저장소를 만든 뒤 프로젝트 루트에서 다음을 실행합니다.

```bash
git init
git add .
git commit -m "Network programming final messenger project"
git branch -M main
git remote add origin https://github.com/<본인아이디>/<저장소명>.git
git push -u origin main
```

이미 저장소를 만든 상태라면 `remote add` 부분만 본인 저장소 주소로 바꾸면 됩니다.

## 10. 구현상 한계와 개선 방향

- 현재는 수업용 단일 프로세스 서버이며 대규모 트래픽용 비동기 이벤트 루프 구조는 아닙니다.
- WebSocket 연결 정보는 메모리에만 보관합니다. 서버 재시작 시 온라인 상태는 초기화됩니다.
- 파일/사진 업로드, 읽음 수, 푸시 알림, 그룹 채팅 초대, 친구 요청 수락 흐름은 확장 과제로 남겼습니다.
- HTTPS 배포 시에는 리버스 프록시 또는 TLS 종료 구성을 추가하는 것이 좋습니다.
