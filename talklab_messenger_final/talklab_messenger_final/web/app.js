const $ = (id) => document.getElementById(id);

const loginView = $("loginView");
const signupView = $("signupView");
const appView = $("appView");

const loginForm = $("loginForm");
const loginId = $("loginId");
const loginPw = $("loginPw");
const loginBtn = $("loginBtn");
const rememberMe = $("rememberMe");
const loginMessage = $("loginMessage");

const signupForm = $("signupForm");
const signupName = $("signupName");
const signupId = $("signupId");
const signupPw = $("signupPw");
const signupMessage = $("signupMessage");

const wsDot = $("wsDot");
const wsStatus = $("wsStatus");
const friendList = $("friendList");
const roomList = $("roomList");
const messageList = $("messageList");

const state = {
  user: null,
  socket: null,
  friends: [],
  rooms: [],
  activeRoom: null,
  deferredInstallPrompt: null,
};

function setMessage(el, text, ok = false) {
  el.textContent = text || "";
  el.style.color = ok ? "#28612f" : "#8a3c35";
}

function checkLoginReady() {
  loginBtn.disabled = !(loginId.value.trim() && loginPw.value.trim());
}

function showAuth(view) {
  loginView.hidden = view !== "login";
  signupView.hidden = view !== "signup";
  appView.hidden = true;
}

function showApp() {
  loginView.hidden = true;
  signupView.hidden = true;
  appView.hidden = false;
}

function tokenStorage() {
  return rememberMe.checked ? localStorage : sessionStorage;
}

function saveToken(token) {
  localStorage.removeItem("talklab_token");
  sessionStorage.removeItem("talklab_token");
  tokenStorage().setItem("talklab_token", token);
}

function getToken() {
  return localStorage.getItem("talklab_token") || sessionStorage.getItem("talklab_token") || "";
}

function clearToken() {
  localStorage.removeItem("talklab_token");
  sessionStorage.removeItem("talklab_token");
}

async function api(path, options = {}) {
  const headers = { ...(options.headers || {}) };
  headers["Content-Type"] = "application/json";
  const token = getToken();
  if (token) headers.Authorization = `Bearer ${token}`;

  const res = await fetch(path, { ...options, headers });
  const data = await res.json().catch(() => ({}));
  if (!res.ok) {
    throw new Error(data.message || `요청 실패: ${res.status}`);
  }
  return data;
}

function formatTime(value) {
  if (!value) return "";
  const normalized = value.includes("T") ? value : value.replace(" ", "T") + "Z";
  const date = new Date(normalized);
  if (Number.isNaN(date.getTime())) return value;
  return date.toLocaleString("ko-KR", { hour: "2-digit", minute: "2-digit", month: "2-digit", day: "2-digit" });
}

function setWsState(stateName, text) {
  wsDot.classList.remove("on", "off");
  if (stateName === "on") wsDot.classList.add("on");
  if (stateName === "off") wsDot.classList.add("off");
  wsStatus.textContent = text;
}

function connectWebSocket(token) {
  if (state.socket) state.socket.close();

  const protocol = location.protocol === "https:" ? "wss" : "ws";
  const socket = new WebSocket(`${protocol}://${location.host}/ws?token=${encodeURIComponent(token)}`);
  state.socket = socket;
  setWsState("", "WebSocket 연결 중...");

  socket.addEventListener("open", () => {
    setWsState("on", "실시간 연결됨");
    socket.send(JSON.stringify({ type: "ping" }));
  });

  socket.addEventListener("message", async (event) => {
    let data;
    try {
      data = JSON.parse(event.data);
    } catch {
      return;
    }

    if (data.type === "welcome") {
      setWsState("on", "실시간 연결됨");
      return;
    }
    if (data.type === "pong") return;
    if (data.type === "error") {
      setWsState("off", data.message || "WebSocket 오류");
      return;
    }
    if (data.type === "message") {
      await handleIncomingMessage(data.message);
    }
  });

  socket.addEventListener("close", () => {
    if (state.socket === socket) setWsState("off", "실시간 연결 종료");
  });

  socket.addEventListener("error", () => {
    if (state.socket === socket) setWsState("off", "WebSocket 오류");
  });
}

function switchTab(tabName) {
  const isFriends = tabName === "friends";
  $("tabFriends").classList.toggle("active", isFriends);
  $("tabChats").classList.toggle("active", !isFriends);
  $("friendsPanel").hidden = !isFriends;
  $("chatsPanel").hidden = isFriends;
}

function avatarText(name) {
  return (name || "?").trim().slice(0, 1).toUpperCase();
}

function renderFriends() {
  friendList.innerHTML = "";
  if (!state.friends.length) {
    friendList.innerHTML = '<p class="empty-line">아직 친구가 없습니다. 상대 아이디를 입력해 추가하세요.</p>';
    return;
  }

  state.friends.forEach((friend) => {
    const row = document.createElement("div");
    row.className = "list-row";

    const avatar = document.createElement("span");
    avatar.className = "avatar";
    avatar.textContent = avatarText(friend.display_name);

    const meta = document.createElement("div");
    meta.className = "row-meta";
    const name = document.createElement("strong");
    name.textContent = friend.display_name;
    const id = document.createElement("p");
    id.textContent = friend.user_id;
    meta.append(name, id);

    const btn = document.createElement("button");
    btn.className = "small-btn secondary";
    btn.type = "button";
    btn.textContent = "채팅";
    btn.addEventListener("click", () => openDirectRoom(friend.user_id));

    row.append(avatar, meta, btn);
    friendList.appendChild(row);
  });
}

function renderRooms() {
  roomList.innerHTML = "";
  if (!state.rooms.length) {
    roomList.innerHTML = '<p class="empty-line">생성된 채팅방이 없습니다.</p>';
    return;
  }

  state.rooms.forEach((room) => {
    const row = document.createElement("button");
    row.className = "list-row room-row";
    row.type = "button";
    if (state.activeRoom && state.activeRoom.id === room.id) row.classList.add("active");

    const avatar = document.createElement("span");
    avatar.className = "avatar";
    avatar.textContent = avatarText(room.display_title);

    const meta = document.createElement("div");
    meta.className = "row-meta";
    const nameLine = document.createElement("div");
    nameLine.className = "room-name-line";
    const name = document.createElement("strong");
    name.textContent = room.display_title;
    const time = document.createElement("span");
    time.textContent = formatTime(room.last_message_at || room.created_at);
    nameLine.append(name, time);

    const last = document.createElement("p");
    last.textContent = room.last_message || `${room.member_count}명 참여 중`;
    meta.append(nameLine, last);
    row.append(avatar, meta);

    row.addEventListener("click", () => openRoom(room));
    roomList.appendChild(row);
  });
}

async function loadFriends() {
  const data = await api("/api/friends");
  state.friends = data.friends || [];
  renderFriends();
}

async function loadRooms() {
  const data = await api("/api/rooms");
  state.rooms = data.rooms || [];
  if (state.activeRoom) {
    const fresh = state.rooms.find((room) => room.id === state.activeRoom.id);
    if (fresh) state.activeRoom = fresh;
  }
  renderRooms();
}

async function openDirectRoom(friendUserId) {
  setMessage($("friendMessage"), "채팅방을 여는 중...", true);
  const data = await api("/api/rooms/direct", {
    method: "POST",
    body: JSON.stringify({ friend_user_id: friendUserId }),
  });
  await loadRooms();
  switchTab("chats");
  openRoom(data.room);
  setMessage($("friendMessage"), "");
}

async function openRoom(room) {
  state.activeRoom = room;
  $("emptyChat").hidden = true;
  $("chatRoom").hidden = false;
  $("chatTitle").textContent = room.display_title;
  $("chatMeta").textContent = room.is_direct ? "1:1 채팅" : `${room.member_count}명 채팅`;
  renderRooms();
  await loadMessages(room.id);
  $("messageInput").focus();
}

async function loadMessages(roomId) {
  messageList.innerHTML = '<p class="empty-line">메시지를 불러오는 중...</p>';
  const data = await api(`/api/rooms/${roomId}/messages?limit=120`);
  messageList.innerHTML = "";
  const messages = data.messages || [];
  if (!messages.length) {
    messageList.innerHTML = '<p class="empty-line">아직 메시지가 없습니다. 첫 메시지를 보내세요.</p>';
    return;
  }
  messages.forEach(appendMessage);
  scrollMessagesToBottom();
}

function scrollMessagesToBottom() {
  messageList.scrollTop = messageList.scrollHeight;
}

function appendMessage(message) {
  if (messageList.querySelector(`[data-message-id="${message.id}"]`)) return;
  const isMine = state.user && message.sender && message.sender.id === state.user.id;

  const item = document.createElement("div");
  item.className = `message-item ${isMine ? "mine" : "theirs"}`;
  item.dataset.messageId = message.id;

  if (!isMine) {
    const sender = document.createElement("span");
    sender.className = "sender-name";
    sender.textContent = message.sender.display_name;
    item.appendChild(sender);
  }

  const bubble = document.createElement("div");
  bubble.className = "message-bubble";
  bubble.textContent = message.body;

  const time = document.createElement("span");
  time.className = "message-time";
  time.textContent = formatTime(message.created_at);

  item.append(bubble, time);
  messageList.appendChild(item);
}

async function handleIncomingMessage(message) {
  const roomId = message.room_id;
  const room = state.rooms.find((item) => item.id === roomId);
  if (room) {
    room.last_message = message.body;
    room.last_message_at = message.created_at;
    state.rooms = [room, ...state.rooms.filter((item) => item.id !== roomId)];
    renderRooms();
  } else {
    await loadRooms();
  }

  if (state.activeRoom && state.activeRoom.id === roomId) {
    if (messageList.querySelector(".empty-line")) messageList.innerHTML = "";
    appendMessage(message);
    scrollMessagesToBottom();
  }
}

async function sendMessage(text) {
  if (!state.activeRoom) return;
  const payload = { type: "chat", room_id: state.activeRoom.id, text };

  if (state.socket && state.socket.readyState === WebSocket.OPEN) {
    state.socket.send(JSON.stringify(payload));
    return;
  }

  const data = await api("/api/messages", {
    method: "POST",
    body: JSON.stringify({ room_id: state.activeRoom.id, text }),
  });
  await handleIncomingMessage(data.message);
}

async function enterApp(user, token) {
  state.user = user;
  $("currentName").textContent = user.display_name;
  $("currentId").textContent = user.user_id;
  showApp();
  connectWebSocket(token);
  await Promise.all([loadFriends(), loadRooms()]);
}

loginId.addEventListener("input", checkLoginReady);
loginPw.addEventListener("input", checkLoginReady);

$("openSignup").addEventListener("click", () => {
  setMessage(loginMessage, "");
  setMessage(signupMessage, "");
  showAuth("signup");
  signupName.focus();
});

$("backToLogin").addEventListener("click", () => {
  showAuth("login");
  loginId.focus();
});

loginForm.addEventListener("submit", async (event) => {
  event.preventDefault();
  setMessage(loginMessage, "로그인 중...", true);
  loginBtn.disabled = true;

  try {
    const data = await api("/api/login", {
      method: "POST",
      body: JSON.stringify({
        user_id: loginId.value.trim(),
        password: loginPw.value,
      }),
    });

    saveToken(data.token);
    await enterApp(data.user, data.token);
    setMessage(loginMessage, "");
  } catch (err) {
    setMessage(loginMessage, err.message);
  } finally {
    checkLoginReady();
  }
});

signupForm.addEventListener("submit", async (event) => {
  event.preventDefault();
  setMessage(signupMessage, "가입 처리 중...", true);

  try {
    await api("/api/register", {
      method: "POST",
      body: JSON.stringify({
        display_name: signupName.value.trim(),
        user_id: signupId.value.trim(),
        password: signupPw.value,
      }),
    });

    setMessage(signupMessage, "가입 완료. 로그인 화면으로 이동합니다.", true);
    loginId.value = signupId.value.trim();
    loginPw.value = "";
    checkLoginReady();
    setTimeout(() => {
      showAuth("login");
      loginPw.focus();
      setMessage(loginMessage, "방금 만든 계정으로 로그인해 주세요.", true);
    }, 500);
  } catch (err) {
    setMessage(signupMessage, err.message);
  }
});

$("logoutBtn").addEventListener("click", async () => {
  try {
    await api("/api/logout", { method: "POST", body: "{}" });
  } catch {}

  if (state.socket) state.socket.close();
  clearToken();
  state.user = null;
  state.friends = [];
  state.rooms = [];
  state.activeRoom = null;
  messageList.innerHTML = "";
  $("chatRoom").hidden = true;
  $("emptyChat").hidden = false;
  setMessage(loginMessage, "로그아웃되었습니다.", true);
  showAuth("login");
});

$("tabFriends").addEventListener("click", () => switchTab("friends"));
$("tabChats").addEventListener("click", () => switchTab("chats"));

$("addFriendForm").addEventListener("submit", async (event) => {
  event.preventDefault();
  const friendUserId = $("friendIdInput").value.trim();
  if (!friendUserId) return;

  setMessage($("friendMessage"), "친구 추가 중...", true);
  try {
    await api("/api/friends", {
      method: "POST",
      body: JSON.stringify({ friend_user_id: friendUserId }),
    });
    $("friendIdInput").value = "";
    setMessage($("friendMessage"), "친구가 추가되었습니다.", true);
    await loadFriends();
  } catch (err) {
    setMessage($("friendMessage"), err.message);
  }
});

$("messageForm").addEventListener("submit", async (event) => {
  event.preventDefault();
  const input = $("messageInput");
  const text = input.value.trim();
  if (!text) return;

  input.value = "";
  try {
    await sendMessage(text);
  } catch (err) {
    input.value = text;
    setWsState("off", err.message);
  }
});

$("refreshBtn").addEventListener("click", async () => {
  await Promise.all([loadRooms(), state.activeRoom ? loadMessages(state.activeRoom.id) : Promise.resolve()]);
});

window.addEventListener("beforeinstallprompt", (event) => {
  event.preventDefault();
  state.deferredInstallPrompt = event;
  $("installBtn").hidden = false;
});

$("installBtn").addEventListener("click", async () => {
  if (!state.deferredInstallPrompt) return;
  state.deferredInstallPrompt.prompt();
  await state.deferredInstallPrompt.userChoice;
  state.deferredInstallPrompt = null;
  $("installBtn").hidden = true;
});

async function boot() {
  checkLoginReady();

  if ("serviceWorker" in navigator) {
    navigator.serviceWorker.register("/sw.js").catch(() => {});
  }

  const token = getToken();
  if (!token) {
    showAuth("login");
    return;
  }

  try {
    const data = await api("/api/me");
    await enterApp(data.user, token);
  } catch {
    clearToken();
    showAuth("login");
  }
}

boot();
