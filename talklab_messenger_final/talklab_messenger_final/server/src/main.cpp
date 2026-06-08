#include "database.h"

#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/json.hpp>
#include <boost/json/src.hpp>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
namespace net = boost::asio;
using tcp = net::ip::tcp;
namespace bj = boost::json;

using Request = http::request<http::string_body>;
using Response = http::response<http::string_body>;

std::string now_iso() {
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

std::string trim(const std::string& s) {
    auto first = std::find_if_not(s.begin(), s.end(), [](unsigned char c) { return std::isspace(c); });
    auto last = std::find_if_not(s.rbegin(), s.rend(), [](unsigned char c) { return std::isspace(c); }).base();
    if (first >= last) return "";
    return std::string(first, last);
}

struct WsClient {
    int user_pk{};
    std::string user_id;
    std::string display_name;
    websocket::stream<tcp::socket>* ws{};
    std::mutex write_mutex;
};

class ChatHub {
public:
    void add(const std::shared_ptr<WsClient>& client) {
        std::lock_guard<std::mutex> lock(mutex_);
        clients_by_user_[client->user_pk].push_back(client);
    }

    void remove(const std::shared_ptr<WsClient>& client) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = clients_by_user_.find(client->user_pk);
        if (it == clients_by_user_.end()) return;

        auto& list = it->second;
        list.erase(std::remove_if(list.begin(), list.end(), [&](const std::weak_ptr<WsClient>& weak) {
            auto locked = weak.lock();
            return !locked || locked.get() == client.get();
        }), list.end());

        if (list.empty()) clients_by_user_.erase(it);
    }

    void send_json(const std::shared_ptr<WsClient>& client, const bj::value& payload) {
        if (!client || !client->ws) return;
        try {
            std::lock_guard<std::mutex> lock(client->write_mutex);
            client->ws->text(true);
            client->ws->write(net::buffer(bj::serialize(payload)));
        } catch (const std::exception& e) {
            std::cerr << "WebSocket send failed: " << e.what() << "\n";
        }
    }

    void broadcast_to_users(const std::vector<int>& user_ids, const bj::value& payload) {
        std::vector<std::shared_ptr<WsClient>> targets;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (int user_pk : user_ids) {
                auto it = clients_by_user_.find(user_pk);
                if (it == clients_by_user_.end()) continue;

                auto& list = it->second;
                for (auto list_it = list.begin(); list_it != list.end();) {
                    if (auto client = list_it->lock()) {
                        targets.push_back(client);
                        ++list_it;
                    } else {
                        list_it = list.erase(list_it);
                    }
                }
            }
        }

        std::unordered_set<WsClient*> seen;
        for (auto& client : targets) {
            if (seen.insert(client.get()).second) {
                send_json(client, payload);
            }
        }
    }

private:
    std::mutex mutex_;
    std::unordered_map<int, std::vector<std::weak_ptr<WsClient>>> clients_by_user_;
};

struct AppState {
    std::shared_ptr<Database> db;
    std::shared_ptr<ChatHub> hub;
    std::filesystem::path static_dir;
};

bj::object user_json(const UserInfo& user) {
    return {
        {"id", user.id},
        {"user_id", user.user_id},
        {"display_name", user.display_name}
    };
}

bj::object friend_json(const FriendInfo& f) {
    return {
        {"id", f.id},
        {"user_id", f.user_id},
        {"display_name", f.display_name},
        {"created_at", f.created_at}
    };
}

bj::object room_json(const RoomInfo& room) {
    return {
        {"id", room.id},
        {"is_direct", room.is_direct},
        {"title", room.title},
        {"display_title", room.display_title},
        {"last_message", room.last_message},
        {"last_message_at", room.last_message_at},
        {"created_at", room.created_at},
        {"member_count", room.member_count}
    };
}

bj::object message_json(const MessageInfo& message) {
    return {
        {"id", message.id},
        {"room_id", message.room_id},
        {"sender", {
            {"id", message.sender_pk},
            {"user_id", message.sender_user_id},
            {"display_name", message.sender_display_name}
        }},
        {"body", message.body},
        {"created_at", message.created_at}
    };
}

template <typename T>
bj::array list_json(const std::vector<T>& items, bj::object (*mapper)(const T&)) {
    bj::array arr;
    for (const auto& item : items) arr.push_back(mapper(item));
    return arr;
}

std::string url_decode(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            std::string hex = s.substr(i + 1, 2);
            char c = static_cast<char>(std::strtol(hex.c_str(), nullptr, 16));
            out.push_back(c);
            i += 2;
        } else if (s[i] == '+') {
            out.push_back(' ');
        } else {
            out.push_back(s[i]);
        }
    }
    return out;
}

std::string path_only(const std::string& target) {
    auto pos = target.find('?');
    return pos == std::string::npos ? target : target.substr(0, pos);
}

std::unordered_map<std::string, std::string> parse_query(const std::string& target) {
    std::unordered_map<std::string, std::string> q;
    auto pos = target.find('?');
    if (pos == std::string::npos) return q;
    std::string query = target.substr(pos + 1);
    std::stringstream ss(query);
    std::string item;
    while (std::getline(ss, item, '&')) {
        auto eq = item.find('=');
        if (eq == std::string::npos) {
            q[url_decode(item)] = "";
        } else {
            q[url_decode(item.substr(0, eq))] = url_decode(item.substr(eq + 1));
        }
    }
    return q;
}

std::string beast_to_string(beast::string_view value) {
    return std::string(value.data(), value.size());
}

std::vector<std::string> split_path(const std::string& path) {
    std::vector<std::string> parts;
    std::stringstream ss(path);
    std::string part;
    while (std::getline(ss, part, '/')) {
        if (!part.empty()) parts.push_back(part);
    }
    return parts;
}

std::string bearer_token(const Request& req) {
    auto it = req.find(http::field::authorization);
    if (it == req.end()) return "";
    std::string auth = beast_to_string(it->value());
    const std::string prefix = "Bearer ";
    if (auth.rfind(prefix, 0) == 0) {
        return auth.substr(prefix.size());
    }
    return "";
}

void add_common_headers(Response& res) {
    res.set(http::field::server, "TalkLab-Cpp-Beast");
    res.set(http::field::access_control_allow_origin, "*");
    res.set(http::field::access_control_allow_methods, "GET, POST, OPTIONS");
    res.set(http::field::access_control_allow_headers, "Content-Type, Authorization");
}

Response json_response(http::status status, const bj::value& body, bool keep_alive = false) {
    Response res{status, 11};
    res.set(http::field::content_type, "application/json; charset=utf-8");
    add_common_headers(res);
    res.keep_alive(keep_alive);
    res.body() = bj::serialize(body);
    res.prepare_payload();
    return res;
}

Response text_response(http::status status, const std::string& body, const std::string& content_type, bool keep_alive = false) {
    Response res{status, 11};
    res.set(http::field::content_type, content_type);
    add_common_headers(res);
    res.keep_alive(keep_alive);
    res.body() = body;
    res.prepare_payload();
    return res;
}

std::string mime_type(const std::filesystem::path& p) {
    auto ext = p.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    if (ext == ".html") return "text/html; charset=utf-8";
    if (ext == ".css") return "text/css; charset=utf-8";
    if (ext == ".js") return "application/javascript; charset=utf-8";
    if (ext == ".json") return "application/json; charset=utf-8";
    if (ext == ".png") return "image/png";
    if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
    if (ext == ".svg") return "image/svg+xml";
    if (ext == ".ico") return "image/x-icon";
    return "application/octet-stream";
}

std::optional<std::string> read_file(const std::filesystem::path& file) {
    std::ifstream ifs(file, std::ios::binary);
    if (!ifs) return std::nullopt;
    std::ostringstream oss;
    oss << ifs.rdbuf();
    return oss.str();
}

Response serve_static(const AppState& state, const std::string& raw_path) {
    std::string rel = raw_path;
    if (rel == "/") rel = "/index.html";
    if (!rel.empty() && rel[0] == '/') rel.erase(rel.begin());
    if (rel.find("..") != std::string::npos) {
        return text_response(http::status::bad_request, "Bad path", "text/plain; charset=utf-8");
    }
    auto file = state.static_dir / rel;
    if (!std::filesystem::exists(file) || std::filesystem::is_directory(file)) {
        file = state.static_dir / "index.html";
    }
    auto content = read_file(file);
    if (!content) {
        return text_response(http::status::not_found, "Not found", "text/plain; charset=utf-8");
    }
    return text_response(http::status::ok, *content, mime_type(file));
}

std::optional<UserInfo> require_user(const AppState& state, const Request& req) {
    return state.db->user_by_token(bearer_token(req));
}

std::string json_string(const bj::object& obj, const std::string& key, const std::string& fallback = "") {
    auto it = obj.find(key);
    if (it == obj.end()) return fallback;
    const auto& value = it->value();
    if (!value.is_string()) return fallback;
    const auto& s = value.as_string();
    return std::string(s.data(), s.size());
}

int json_int(const bj::object& obj, const std::string& key, int fallback = 0) {
    auto it = obj.find(key);
    if (it == obj.end()) return fallback;
    const auto& value = it->value();
    try {
        if (value.is_int64()) return static_cast<int>(value.as_int64());
        if (value.is_uint64()) return static_cast<int>(value.as_uint64());
        if (value.is_double()) return static_cast<int>(value.as_double());
        if (value.is_string()) {
            const auto& s = value.as_string();
            return std::stoi(std::string(s.data(), s.size()));
        }
    } catch (...) {
        return fallback;
    }
    return fallback;
}

bj::object parse_body_object(const Request& req) {
    if (req.body().empty()) return {};
    bj::value parsed = bj::parse(req.body());
    if (!parsed.is_object()) return {};
    return parsed.as_object();
}

bj::object message_event_json(const MessageInfo& message) {
    return {
        {"type", "message"},
        {"message", message_json(message)},
        {"time", now_iso()}
    };
}

Response handle_api(const AppState& state, const Request& req, const std::string& path) {
    try {
        if (req.method() == http::verb::options) {
            Response res{http::status::no_content, 11};
            add_common_headers(res);
            res.prepare_payload();
            return res;
        }

        if (path == "/api/health" && req.method() == http::verb::get) {
            return json_response(http::status::ok, { {"ok", true}, {"time", now_iso()}, {"service", "TalkLab Messenger"} });
        }

        if (path == "/api/register" && req.method() == http::verb::post) {
            auto body = parse_body_object(req);
            std::string user_id = trim(json_string(body, "user_id", ""));
            std::string display_name = trim(json_string(body, "display_name", ""));
            std::string password = json_string(body, "password", "");

            std::string err;
            if (!state.db->create_user(user_id, display_name, password, err)) {
                return json_response(http::status::bad_request, { {"ok", false}, {"message", err} });
            }
            return json_response(http::status::created, { {"ok", true}, {"message", "가입이 완료되었습니다."} });
        }

        if (path == "/api/login" && req.method() == http::verb::post) {
            auto body = parse_body_object(req);
            std::string user_id = trim(json_string(body, "user_id", ""));
            std::string password = json_string(body, "password", "");

            auto user = state.db->verify_login(user_id, password);
            if (!user) {
                return json_response(http::status::unauthorized, { {"ok", false}, {"message", "아이디 또는 비밀번호가 올바르지 않습니다."} });
            }

            std::string token = state.db->create_session(user->id);
            return json_response(http::status::ok, {
                {"ok", true},
                {"token", token},
                {"user", user_json(*user)}
            });
        }

        if (path == "/api/me" && req.method() == http::verb::get) {
            auto user = require_user(state, req);
            if (!user) {
                return json_response(http::status::unauthorized, { {"ok", false}, {"message", "로그인이 필요합니다."} });
            }
            return json_response(http::status::ok, { {"ok", true}, {"user", user_json(*user)} });
        }

        if (path == "/api/logout" && req.method() == http::verb::post) {
            auto token = bearer_token(req);
            if (!token.empty()) state.db->delete_session(token);
            return json_response(http::status::ok, { {"ok", true}, {"message", "로그아웃되었습니다."} });
        }

        auto user = require_user(state, req);
        if (!user) {
            return json_response(http::status::unauthorized, { {"ok", false}, {"message", "로그인이 필요합니다."} });
        }

        if (path == "/api/users" && req.method() == http::verb::get) {
            auto q = parse_query(beast_to_string(req.target()));
            std::string keyword = q.count("q") ? trim(q["q"]) : "";
            auto users = state.db->search_users(user->id, keyword, 30);
            return json_response(http::status::ok, { {"ok", true}, {"users", list_json<UserInfo>(users, user_json)} });
        }

        if (path == "/api/friends" && req.method() == http::verb::get) {
            auto friends = state.db->list_friends(user->id);
            return json_response(http::status::ok, { {"ok", true}, {"friends", list_json<FriendInfo>(friends, friend_json)} });
        }

        if (path == "/api/friends" && req.method() == http::verb::post) {
            auto body = parse_body_object(req);
            std::string friend_user_id = trim(json_string(body, "friend_user_id", ""));
            std::string err;
            if (!state.db->add_friend(user->id, friend_user_id, err)) {
                return json_response(http::status::bad_request, { {"ok", false}, {"message", err} });
            }
            return json_response(http::status::ok, { {"ok", true}, {"message", "친구가 추가되었습니다."} });
        }

        if (path == "/api/rooms" && req.method() == http::verb::get) {
            auto rooms = state.db->list_rooms(user->id);
            return json_response(http::status::ok, { {"ok", true}, {"rooms", list_json<RoomInfo>(rooms, room_json)} });
        }

        if (path == "/api/rooms/direct" && req.method() == http::verb::post) {
            auto body = parse_body_object(req);
            std::string friend_user_id = trim(json_string(body, "friend_user_id", ""));
            std::string err;
            auto room = state.db->create_direct_room(user->id, friend_user_id, err);
            if (!room) {
                return json_response(http::status::bad_request, { {"ok", false}, {"message", err} });
            }
            return json_response(http::status::ok, { {"ok", true}, {"room", room_json(*room)} });
        }

        if (path == "/api/messages" && req.method() == http::verb::post) {
            auto body = parse_body_object(req);
            int room_id = json_int(body, "room_id", 0);
            std::string text = trim(json_string(body, "text", ""));
            std::string err;
            auto message = state.db->create_message(user->id, room_id, text, err);
            if (!message) {
                return json_response(http::status::bad_request, { {"ok", false}, {"message", err} });
            }
            auto member_ids = state.db->room_member_ids(message->room_id);
            state.hub->broadcast_to_users(member_ids, message_event_json(*message));
            return json_response(http::status::created, { {"ok", true}, {"message", message_json(*message)} });
        }

        auto parts = split_path(path);
        if (parts.size() == 4 && parts[0] == "api" && parts[1] == "rooms" && parts[3] == "messages" && req.method() == http::verb::get) {
            int room_id = std::stoi(parts[2]);
            auto q = parse_query(beast_to_string(req.target()));
            int limit = 80;
            if (q.count("limit")) {
                try { limit = std::stoi(q["limit"]); } catch (...) { limit = 80; }
            }
            auto messages = state.db->list_messages(user->id, room_id, limit);
            if (!state.db->is_room_member(user->id, room_id)) {
                return json_response(http::status::forbidden, { {"ok", false}, {"message", "채팅방 접근 권한이 없습니다."} });
            }
            return json_response(http::status::ok, { {"ok", true}, {"messages", list_json<MessageInfo>(messages, message_json)} });
        }

        return json_response(http::status::not_found, { {"ok", false}, {"message", "API not found"} });
    } catch (const boost::system::system_error&) {
        return json_response(http::status::bad_request, { {"ok", false}, {"message", "JSON 형식이 올바르지 않습니다."} });
    } catch (const std::invalid_argument&) {
        return json_response(http::status::bad_request, { {"ok", false}, {"message", "요청 경로의 숫자 값이 올바르지 않습니다."} });
    } catch (const std::out_of_range&) {
        return json_response(http::status::bad_request, { {"ok", false}, {"message", "요청 값의 범위가 올바르지 않습니다."} });
    } catch (const std::exception& e) {
        return json_response(http::status::internal_server_error, { {"ok", false}, {"message", e.what()} });
    }
}

Response handle_request(const AppState& state, const Request& req) {
    std::string path = path_only(beast_to_string(req.target()));
    if (path.rfind("/api/", 0) == 0) {
        return handle_api(state, req, path);
    }
    return serve_static(state, path);
}

void websocket_session(tcp::socket socket, Request req, AppState state) {
    std::shared_ptr<WsClient> client;
    try {
        websocket::stream<tcp::socket> ws(std::move(socket));
        ws.set_option(websocket::stream_base::timeout::suggested(beast::role_type::server));
        ws.set_option(websocket::stream_base::decorator([](websocket::response_type& res) {
            res.set(http::field::server, "TalkLab-Cpp-WebSocket");
        }));
        ws.accept(req);

        auto q = parse_query(beast_to_string(req.target()));
        std::string token = q.count("token") ? q["token"] : "";
        auto user = state.db->user_by_token(token);
        if (!user) {
            ws.text(true);
            ws.write(net::buffer(bj::serialize(bj::object{ {"type", "error"}, {"message", "로그인이 필요합니다."} })));
            ws.close(websocket::close_code::normal);
            return;
        }

        client = std::make_shared<WsClient>();
        client->user_pk = user->id;
        client->user_id = user->user_id;
        client->display_name = user->display_name;
        client->ws = &ws;
        state.hub->add(client);

        state.hub->send_json(client, {
            {"type", "welcome"},
            {"message", "WebSocket 연결 성공"},
            {"user", user_json(*user)},
            {"time", now_iso()}
        });

        beast::flat_buffer buffer;
        while (true) {
            buffer.clear();
            ws.read(buffer);
            std::string raw = beast::buffers_to_string(buffer.data());

            bj::value incoming_value;
            try {
                incoming_value = bj::parse(raw);
            } catch (...) {
                state.hub->send_json(client, { {"type", "error"}, {"message", "WebSocket 메시지는 JSON이어야 합니다."} });
                continue;
            }

            if (!incoming_value.is_object()) {
                state.hub->send_json(client, { {"type", "error"}, {"message", "WebSocket 메시지는 JSON 객체여야 합니다."} });
                continue;
            }
            auto& incoming = incoming_value.as_object();
            std::string type = json_string(incoming, "type", "");
            if (type == "ping") {
                state.hub->send_json(client, { {"type", "pong"}, {"time", now_iso()} });
                continue;
            }

            if (type == "chat") {
                int room_id = json_int(incoming, "room_id", 0);
                std::string text = trim(json_string(incoming, "text", ""));
                std::string err;
                auto message = state.db->create_message(user->id, room_id, text, err);
                if (!message) {
                    state.hub->send_json(client, { {"type", "error"}, {"message", err} });
                    continue;
                }

                auto member_ids = state.db->room_member_ids(message->room_id);
                state.hub->broadcast_to_users(member_ids, message_event_json(*message));
                continue;
            }

            state.hub->send_json(client, {
                {"type", "error"},
                {"message", "지원하지 않는 WebSocket 타입입니다."},
                {"received_type", type}
            });
        }
    } catch (const beast::system_error& se) {
        if (se.code() != websocket::error::closed) {
            std::cerr << "WebSocket error: " << se.code().message() << "\n";
        }
    } catch (const std::exception& e) {
        std::cerr << "WebSocket exception: " << e.what() << "\n";
    }

    if (client) {
        state.hub->remove(client);
    }
}

void http_session(tcp::socket socket, AppState state) {
    try {
        beast::flat_buffer buffer;
        Request req;
        http::read(socket, buffer, req);

        if (websocket::is_upgrade(req) && path_only(beast_to_string(req.target())) == "/ws") {
            websocket_session(std::move(socket), std::move(req), std::move(state));
            return;
        }

        Response res = handle_request(state, req);
        http::write(socket, res);
        beast::error_code ec;
        socket.shutdown(tcp::socket::shutdown_send, ec);
    } catch (const std::exception& e) {
        std::cerr << "HTTP session error: " << e.what() << "\n";
    }
}

int main(int argc, char* argv[]) {
    try {
        unsigned short port = 8080;
        if (argc >= 2) port = static_cast<unsigned short>(std::stoi(argv[1]));
        std::filesystem::path static_dir = argc >= 3 ? argv[2] : "web";
        std::string db_path = argc >= 4 ? argv[3] : "data/talklab.db";

        AppState state;
        state.static_dir = static_dir;
        state.db = std::make_shared<Database>(db_path);
        state.hub = std::make_shared<ChatHub>();
        state.db->init();

        net::io_context ioc{1};
        tcp::acceptor acceptor{ioc, {tcp::v4(), port}};

        std::cout << "TalkLab server started\n";
        std::cout << "  HTTP      : http://localhost:" << port << "\n";
        std::cout << "  WebSocket : ws://localhost:" << port << "/ws?token=...\n";
        std::cout << "  Static dir: " << std::filesystem::absolute(static_dir) << "\n";
        std::cout << "  DB        : " << std::filesystem::absolute(db_path) << "\n";

        while (true) {
            tcp::socket socket{ioc};
            acceptor.accept(socket);
            std::thread{http_session, std::move(socket), state}.detach();
        }
    } catch (const std::exception& e) {
        std::cerr << "Fatal: " << e.what() << "\n";
        return 1;
    }
}
