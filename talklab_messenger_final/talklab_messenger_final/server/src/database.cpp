#include "database.h"
#include "security.h"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <stdexcept>

namespace {
void exec_or_throw(sqlite3* db, const char* sql) {
    char* err = nullptr;
    if (sqlite3_exec(db, sql, nullptr, nullptr, &err) != SQLITE_OK) {
        std::string msg = err ? err : "sqlite error";
        sqlite3_free(err);
        throw std::runtime_error(msg);
    }
}

std::string column_text(sqlite3_stmt* stmt, int col) {
    const unsigned char* text = sqlite3_column_text(stmt, col);
    return text ? reinterpret_cast<const char*>(text) : "";
}

void finalize(sqlite3_stmt* stmt) {
    if (stmt) sqlite3_finalize(stmt);
}

int clamp_limit(int value, int min_value, int max_value) {
    return std::max(min_value, std::min(value, max_value));
}
}

Database::Database(const std::string& path) {
    std::filesystem::path db_path(path);
    if (db_path.has_parent_path()) {
        std::filesystem::create_directories(db_path.parent_path());
    }

    if (sqlite3_open(path.c_str(), &db_) != SQLITE_OK) {
        std::string msg = sqlite3_errmsg(db_);
        sqlite3_close(db_);
        db_ = nullptr;
        throw std::runtime_error("Cannot open database: " + msg);
    }
    exec_or_throw(db_, "PRAGMA foreign_keys = ON;");
    exec_or_throw(db_, "PRAGMA journal_mode = WAL;");
}

Database::~Database() {
    if (db_) sqlite3_close(db_);
}

void Database::init() {
    std::lock_guard<std::mutex> lock(mutex_);

    exec_or_throw(db_, R"SQL(
        CREATE TABLE IF NOT EXISTS users (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            user_id TEXT NOT NULL UNIQUE,
            display_name TEXT NOT NULL,
            password_hash TEXT NOT NULL,
            salt TEXT NOT NULL,
            created_at TEXT NOT NULL DEFAULT (datetime('now'))
        );
    )SQL");

    exec_or_throw(db_, R"SQL(
        CREATE TABLE IF NOT EXISTS sessions (
            token TEXT PRIMARY KEY,
            user_pk INTEGER NOT NULL,
            created_at TEXT NOT NULL DEFAULT (datetime('now')),
            FOREIGN KEY(user_pk) REFERENCES users(id) ON DELETE CASCADE
        );
    )SQL");

    exec_or_throw(db_, R"SQL(
        CREATE TABLE IF NOT EXISTS friends (
            user_pk INTEGER NOT NULL,
            friend_pk INTEGER NOT NULL,
            created_at TEXT NOT NULL DEFAULT (datetime('now')),
            PRIMARY KEY(user_pk, friend_pk),
            FOREIGN KEY(user_pk) REFERENCES users(id) ON DELETE CASCADE,
            FOREIGN KEY(friend_pk) REFERENCES users(id) ON DELETE CASCADE
        );
    )SQL");

    exec_or_throw(db_, R"SQL(
        CREATE TABLE IF NOT EXISTS chat_rooms (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            title TEXT,
            is_direct INTEGER NOT NULL DEFAULT 1,
            direct_key TEXT UNIQUE,
            created_by INTEGER NOT NULL,
            created_at TEXT NOT NULL DEFAULT (datetime('now')),
            last_message TEXT NOT NULL DEFAULT '',
            last_message_at TEXT,
            FOREIGN KEY(created_by) REFERENCES users(id) ON DELETE CASCADE
        );
    )SQL");

    exec_or_throw(db_, R"SQL(
        CREATE TABLE IF NOT EXISTS room_members (
            room_id INTEGER NOT NULL,
            user_pk INTEGER NOT NULL,
            joined_at TEXT NOT NULL DEFAULT (datetime('now')),
            PRIMARY KEY(room_id, user_pk),
            FOREIGN KEY(room_id) REFERENCES chat_rooms(id) ON DELETE CASCADE,
            FOREIGN KEY(user_pk) REFERENCES users(id) ON DELETE CASCADE
        );
    )SQL");

    exec_or_throw(db_, R"SQL(
        CREATE TABLE IF NOT EXISTS messages (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            room_id INTEGER NOT NULL,
            sender_pk INTEGER NOT NULL,
            body TEXT NOT NULL,
            created_at TEXT NOT NULL DEFAULT (datetime('now')),
            FOREIGN KEY(room_id) REFERENCES chat_rooms(id) ON DELETE CASCADE,
            FOREIGN KEY(sender_pk) REFERENCES users(id) ON DELETE CASCADE
        );
    )SQL");

    exec_or_throw(db_, "CREATE INDEX IF NOT EXISTS idx_friends_user ON friends(user_pk);");
    exec_or_throw(db_, "CREATE INDEX IF NOT EXISTS idx_room_members_user ON room_members(user_pk);");
    exec_or_throw(db_, "CREATE INDEX IF NOT EXISTS idx_messages_room_id ON messages(room_id, id DESC);");
}

bool Database::create_user(const std::string& user_id,
                           const std::string& display_name,
                           const std::string& password,
                           std::string& error_message) {
    if (user_id.size() < 4 || user_id.size() > 40) {
        error_message = "아이디는 4~40자로 입력해 주세요.";
        return false;
    }
    if (password.size() < 6 || password.size() > 100) {
        error_message = "비밀번호는 6자 이상 입력해 주세요.";
        return false;
    }
    if (display_name.empty() || display_name.size() > 40) {
        error_message = "표시 이름을 입력해 주세요.";
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);

    std::string salt = security::random_hex(16);
    std::string hash = security::pbkdf2_sha256_hex(password, salt);

    sqlite3_stmt* stmt = nullptr;
    const char* sql = "INSERT INTO users(user_id, display_name, password_hash, salt) VALUES(?, ?, ?, ?);";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        error_message = sqlite3_errmsg(db_);
        return false;
    }

    sqlite3_bind_text(stmt, 1, user_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, display_name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, hash.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, salt.c_str(), -1, SQLITE_TRANSIENT);

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        if (rc == SQLITE_CONSTRAINT) {
            error_message = "이미 사용 중인 아이디입니다.";
        } else {
            error_message = sqlite3_errmsg(db_);
        }
        return false;
    }
    return true;
}

std::optional<UserInfo> Database::verify_login(const std::string& user_id,
                                               const std::string& password) {
    std::lock_guard<std::mutex> lock(mutex_);

    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT id, user_id, display_name, password_hash, salt FROM users WHERE user_id = ?;";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return std::nullopt;
    }
    sqlite3_bind_text(stmt, 1, user_id.c_str(), -1, SQLITE_TRANSIENT);

    int rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return std::nullopt;
    }

    int id = sqlite3_column_int(stmt, 0);
    std::string uid = column_text(stmt, 1);
    std::string display = column_text(stmt, 2);
    std::string stored_hash = column_text(stmt, 3);
    std::string salt = column_text(stmt, 4);
    sqlite3_finalize(stmt);

    std::string computed = security::pbkdf2_sha256_hex(password, salt);
    if (!security::constant_time_equal(stored_hash, computed)) {
        return std::nullopt;
    }

    return UserInfo{id, uid, display};
}

std::string Database::create_session(int user_pk) {
    std::lock_guard<std::mutex> lock(mutex_);

    std::string token = security::random_hex(32);
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "INSERT INTO sessions(token, user_pk) VALUES(?, ?);";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        throw std::runtime_error(sqlite3_errmsg(db_));
    }
    sqlite3_bind_text(stmt, 1, token.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, user_pk);

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        throw std::runtime_error(sqlite3_errmsg(db_));
    }
    return token;
}

std::optional<UserInfo> Database::user_by_token(const std::string& token) {
    if (token.empty()) return std::nullopt;

    std::lock_guard<std::mutex> lock(mutex_);

    sqlite3_stmt* stmt = nullptr;
    const char* sql = R"SQL(
        SELECT users.id, users.user_id, users.display_name
        FROM sessions
        JOIN users ON users.id = sessions.user_pk
        WHERE sessions.token = ?;
    )SQL";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return std::nullopt;
    }
    sqlite3_bind_text(stmt, 1, token.c_str(), -1, SQLITE_TRANSIENT);

    int rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return std::nullopt;
    }

    UserInfo user;
    user.id = sqlite3_column_int(stmt, 0);
    user.user_id = column_text(stmt, 1);
    user.display_name = column_text(stmt, 2);
    sqlite3_finalize(stmt);
    return user;
}

void Database::delete_session(const std::string& token) {
    std::lock_guard<std::mutex> lock(mutex_);

    sqlite3_stmt* stmt = nullptr;
    const char* sql = "DELETE FROM sessions WHERE token = ?;";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return;
    sqlite3_bind_text(stmt, 1, token.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

std::optional<UserInfo> Database::user_by_user_id_locked(const std::string& user_id) {
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT id, user_id, display_name FROM users WHERE user_id = ?;";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return std::nullopt;
    sqlite3_bind_text(stmt, 1, user_id.c_str(), -1, SQLITE_TRANSIENT);

    int rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return std::nullopt;
    }

    UserInfo user;
    user.id = sqlite3_column_int(stmt, 0);
    user.user_id = column_text(stmt, 1);
    user.display_name = column_text(stmt, 2);
    sqlite3_finalize(stmt);
    return user;
}

std::vector<UserInfo> Database::search_users(int current_user_pk, const std::string& query, int limit) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<UserInfo> users;

    sqlite3_stmt* stmt = nullptr;
    const char* sql = R"SQL(
        SELECT id, user_id, display_name
        FROM users
        WHERE id <> ?
          AND (? = '' OR user_id LIKE ? ESCAPE '\' OR display_name LIKE ? ESCAPE '\')
        ORDER BY display_name COLLATE NOCASE, user_id COLLATE NOCASE
        LIMIT ?;
    )SQL";

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return users;
    std::string pattern = "%" + query + "%";
    sqlite3_bind_int(stmt, 1, current_user_pk);
    sqlite3_bind_text(stmt, 2, query.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, pattern.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, pattern.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 5, clamp_limit(limit, 1, 50));

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        users.push_back(UserInfo{
            sqlite3_column_int(stmt, 0),
            column_text(stmt, 1),
            column_text(stmt, 2)
        });
    }
    sqlite3_finalize(stmt);
    return users;
}

bool Database::add_friend(int current_user_pk, const std::string& friend_user_id, std::string& error_message) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto friend_user = user_by_user_id_locked(friend_user_id);
    if (!friend_user) {
        error_message = "해당 아이디의 사용자를 찾을 수 없습니다.";
        return false;
    }
    if (friend_user->id == current_user_pk) {
        error_message = "자기 자신은 친구로 추가할 수 없습니다.";
        return false;
    }

    try {
        exec_or_throw(db_, "BEGIN IMMEDIATE;");

        sqlite3_stmt* stmt = nullptr;
        const char* sql = "INSERT OR IGNORE INTO friends(user_pk, friend_pk) VALUES(?, ?);";
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            throw std::runtime_error(sqlite3_errmsg(db_));
        }

        sqlite3_bind_int(stmt, 1, current_user_pk);
        sqlite3_bind_int(stmt, 2, friend_user->id);
        if (sqlite3_step(stmt) != SQLITE_DONE) {
            finalize(stmt);
            throw std::runtime_error(sqlite3_errmsg(db_));
        }
        sqlite3_reset(stmt);
        sqlite3_clear_bindings(stmt);

        sqlite3_bind_int(stmt, 1, friend_user->id);
        sqlite3_bind_int(stmt, 2, current_user_pk);
        if (sqlite3_step(stmt) != SQLITE_DONE) {
            finalize(stmt);
            throw std::runtime_error(sqlite3_errmsg(db_));
        }
        sqlite3_finalize(stmt);

        exec_or_throw(db_, "COMMIT;");
    } catch (const std::exception& e) {
        sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        error_message = e.what();
        return false;
    }

    return true;
}

std::vector<FriendInfo> Database::list_friends(int current_user_pk) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<FriendInfo> friends;

    sqlite3_stmt* stmt = nullptr;
    const char* sql = R"SQL(
        SELECT u.id, u.user_id, u.display_name, f.created_at
        FROM friends f
        JOIN users u ON u.id = f.friend_pk
        WHERE f.user_pk = ?
        ORDER BY u.display_name COLLATE NOCASE, u.user_id COLLATE NOCASE;
    )SQL";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return friends;
    sqlite3_bind_int(stmt, 1, current_user_pk);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        friends.push_back(FriendInfo{
            sqlite3_column_int(stmt, 0),
            column_text(stmt, 1),
            column_text(stmt, 2),
            column_text(stmt, 3)
        });
    }
    sqlite3_finalize(stmt);
    return friends;
}

std::optional<RoomInfo> Database::room_by_id_for_user_locked(int current_user_pk, int room_id) {
    sqlite3_stmt* stmt = nullptr;
    const char* sql = R"SQL(
        SELECT cr.id,
               cr.is_direct,
               COALESCE(cr.title, ''),
               CASE
                   WHEN cr.is_direct = 1 THEN COALESCE((
                       SELECT u.display_name
                       FROM room_members rm
                       JOIN users u ON u.id = rm.user_pk
                       WHERE rm.room_id = cr.id AND rm.user_pk <> ?
                       LIMIT 1
                   ), '나와의 채팅')
                   WHEN COALESCE(cr.title, '') <> '' THEN cr.title
                   ELSE '그룹 채팅'
               END AS display_title,
               cr.last_message,
               COALESCE(cr.last_message_at, ''),
               cr.created_at,
               (SELECT COUNT(*) FROM room_members rm2 WHERE rm2.room_id = cr.id) AS member_count
        FROM chat_rooms cr
        JOIN room_members mine ON mine.room_id = cr.id AND mine.user_pk = ?
        WHERE cr.id = ?;
    )SQL";

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return std::nullopt;
    sqlite3_bind_int(stmt, 1, current_user_pk);
    sqlite3_bind_int(stmt, 2, current_user_pk);
    sqlite3_bind_int(stmt, 3, room_id);

    int rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return std::nullopt;
    }

    RoomInfo room;
    room.id = sqlite3_column_int(stmt, 0);
    room.is_direct = sqlite3_column_int(stmt, 1) != 0;
    room.title = column_text(stmt, 2);
    room.display_title = column_text(stmt, 3);
    room.last_message = column_text(stmt, 4);
    room.last_message_at = column_text(stmt, 5);
    room.created_at = column_text(stmt, 6);
    room.member_count = sqlite3_column_int(stmt, 7);
    sqlite3_finalize(stmt);
    return room;
}

std::optional<RoomInfo> Database::create_direct_room(int current_user_pk,
                                                     const std::string& friend_user_id,
                                                     std::string& error_message) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto friend_user = user_by_user_id_locked(friend_user_id);
    if (!friend_user) {
        error_message = "해당 아이디의 사용자를 찾을 수 없습니다.";
        return std::nullopt;
    }
    if (friend_user->id == current_user_pk) {
        error_message = "자기 자신과의 1:1 채팅방은 만들 수 없습니다.";
        return std::nullopt;
    }

    int a = std::min(current_user_pk, friend_user->id);
    int b = std::max(current_user_pk, friend_user->id);
    std::string direct_key = std::to_string(a) + ":" + std::to_string(b);

    sqlite3_stmt* existing = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT id FROM chat_rooms WHERE direct_key = ?;", -1, &existing, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(existing, 1, direct_key.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(existing) == SQLITE_ROW) {
            int room_id = sqlite3_column_int(existing, 0);
            sqlite3_finalize(existing);
            return room_by_id_for_user_locked(current_user_pk, room_id);
        }
    }
    finalize(existing);

    int room_id = 0;
    try {
        exec_or_throw(db_, "BEGIN IMMEDIATE;");

        sqlite3_stmt* room_stmt = nullptr;
        const char* room_sql = "INSERT INTO chat_rooms(title, is_direct, direct_key, created_by) VALUES(NULL, 1, ?, ?);";
        if (sqlite3_prepare_v2(db_, room_sql, -1, &room_stmt, nullptr) != SQLITE_OK) {
            throw std::runtime_error(sqlite3_errmsg(db_));
        }
        sqlite3_bind_text(room_stmt, 1, direct_key.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(room_stmt, 2, current_user_pk);
        if (sqlite3_step(room_stmt) != SQLITE_DONE) {
            finalize(room_stmt);
            throw std::runtime_error(sqlite3_errmsg(db_));
        }
        sqlite3_finalize(room_stmt);
        room_id = static_cast<int>(sqlite3_last_insert_rowid(db_));

        sqlite3_stmt* member_stmt = nullptr;
        const char* member_sql = "INSERT INTO room_members(room_id, user_pk) VALUES(?, ?);";
        if (sqlite3_prepare_v2(db_, member_sql, -1, &member_stmt, nullptr) != SQLITE_OK) {
            throw std::runtime_error(sqlite3_errmsg(db_));
        }
        sqlite3_bind_int(member_stmt, 1, room_id);
        sqlite3_bind_int(member_stmt, 2, current_user_pk);
        if (sqlite3_step(member_stmt) != SQLITE_DONE) {
            finalize(member_stmt);
            throw std::runtime_error(sqlite3_errmsg(db_));
        }
        sqlite3_reset(member_stmt);
        sqlite3_clear_bindings(member_stmt);

        sqlite3_bind_int(member_stmt, 1, room_id);
        sqlite3_bind_int(member_stmt, 2, friend_user->id);
        if (sqlite3_step(member_stmt) != SQLITE_DONE) {
            finalize(member_stmt);
            throw std::runtime_error(sqlite3_errmsg(db_));
        }
        sqlite3_finalize(member_stmt);

        exec_or_throw(db_, "COMMIT;");
    } catch (const std::exception& e) {
        sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        error_message = e.what();
        return std::nullopt;
    }

    return room_by_id_for_user_locked(current_user_pk, room_id);
}

std::vector<RoomInfo> Database::list_rooms(int current_user_pk) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<RoomInfo> rooms;

    sqlite3_stmt* stmt = nullptr;
    const char* sql = R"SQL(
        SELECT cr.id,
               cr.is_direct,
               COALESCE(cr.title, ''),
               CASE
                   WHEN cr.is_direct = 1 THEN COALESCE((
                       SELECT u.display_name
                       FROM room_members rm
                       JOIN users u ON u.id = rm.user_pk
                       WHERE rm.room_id = cr.id AND rm.user_pk <> ?
                       LIMIT 1
                   ), '나와의 채팅')
                   WHEN COALESCE(cr.title, '') <> '' THEN cr.title
                   ELSE '그룹 채팅'
               END AS display_title,
               cr.last_message,
               COALESCE(cr.last_message_at, ''),
               cr.created_at,
               (SELECT COUNT(*) FROM room_members rm2 WHERE rm2.room_id = cr.id) AS member_count
        FROM chat_rooms cr
        JOIN room_members mine ON mine.room_id = cr.id AND mine.user_pk = ?
        ORDER BY COALESCE(cr.last_message_at, cr.created_at) DESC, cr.id DESC;
    )SQL";

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return rooms;
    sqlite3_bind_int(stmt, 1, current_user_pk);
    sqlite3_bind_int(stmt, 2, current_user_pk);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        RoomInfo room;
        room.id = sqlite3_column_int(stmt, 0);
        room.is_direct = sqlite3_column_int(stmt, 1) != 0;
        room.title = column_text(stmt, 2);
        room.display_title = column_text(stmt, 3);
        room.last_message = column_text(stmt, 4);
        room.last_message_at = column_text(stmt, 5);
        room.created_at = column_text(stmt, 6);
        room.member_count = sqlite3_column_int(stmt, 7);
        rooms.push_back(room);
    }
    sqlite3_finalize(stmt);
    return rooms;
}

bool Database::is_room_member_locked(int user_pk, int room_id) {
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT 1 FROM room_members WHERE user_pk = ? AND room_id = ? LIMIT 1;";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_int(stmt, 1, user_pk);
    sqlite3_bind_int(stmt, 2, room_id);
    bool ok = sqlite3_step(stmt) == SQLITE_ROW;
    sqlite3_finalize(stmt);
    return ok;
}

bool Database::is_room_member(int user_pk, int room_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    return is_room_member_locked(user_pk, room_id);
}

std::vector<int> Database::room_member_ids(int room_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<int> ids;

    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT user_pk FROM room_members WHERE room_id = ?;";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return ids;
    sqlite3_bind_int(stmt, 1, room_id);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        ids.push_back(sqlite3_column_int(stmt, 0));
    }
    sqlite3_finalize(stmt);
    return ids;
}

std::vector<MessageInfo> Database::list_messages(int current_user_pk, int room_id, int limit) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<MessageInfo> messages;

    if (!is_room_member_locked(current_user_pk, room_id)) {
        return messages;
    }

    sqlite3_stmt* stmt = nullptr;
    const char* sql = R"SQL(
        SELECT id, room_id, sender_pk, sender_user_id, sender_display_name, body, created_at
        FROM (
            SELECT m.id,
                   m.room_id,
                   m.sender_pk,
                   u.user_id AS sender_user_id,
                   u.display_name AS sender_display_name,
                   m.body,
                   m.created_at
            FROM messages m
            JOIN users u ON u.id = m.sender_pk
            WHERE m.room_id = ?
            ORDER BY m.id DESC
            LIMIT ?
        )
        ORDER BY id ASC;
    )SQL";

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return messages;
    sqlite3_bind_int(stmt, 1, room_id);
    sqlite3_bind_int(stmt, 2, clamp_limit(limit, 1, 200));

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        messages.push_back(MessageInfo{
            sqlite3_column_int(stmt, 0),
            sqlite3_column_int(stmt, 1),
            sqlite3_column_int(stmt, 2),
            column_text(stmt, 3),
            column_text(stmt, 4),
            column_text(stmt, 5),
            column_text(stmt, 6)
        });
    }
    sqlite3_finalize(stmt);
    return messages;
}

std::optional<MessageInfo> Database::message_by_id_locked(int message_id) {
    sqlite3_stmt* stmt = nullptr;
    const char* sql = R"SQL(
        SELECT m.id,
               m.room_id,
               m.sender_pk,
               u.user_id,
               u.display_name,
               m.body,
               m.created_at
        FROM messages m
        JOIN users u ON u.id = m.sender_pk
        WHERE m.id = ?;
    )SQL";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return std::nullopt;
    sqlite3_bind_int(stmt, 1, message_id);

    if (sqlite3_step(stmt) != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return std::nullopt;
    }

    MessageInfo message{
        sqlite3_column_int(stmt, 0),
        sqlite3_column_int(stmt, 1),
        sqlite3_column_int(stmt, 2),
        column_text(stmt, 3),
        column_text(stmt, 4),
        column_text(stmt, 5),
        column_text(stmt, 6)
    };
    sqlite3_finalize(stmt);
    return message;
}

std::optional<MessageInfo> Database::create_message(int current_user_pk,
                                                    int room_id,
                                                    const std::string& body,
                                                    std::string& error_message) {
    if (body.empty()) {
        error_message = "메시지를 입력해 주세요.";
        return std::nullopt;
    }
    if (body.size() > 2000) {
        error_message = "메시지는 2000자 이내로 입력해 주세요.";
        return std::nullopt;
    }

    std::lock_guard<std::mutex> lock(mutex_);

    if (!is_room_member_locked(current_user_pk, room_id)) {
        error_message = "채팅방 접근 권한이 없습니다.";
        return std::nullopt;
    }

    int message_id = 0;
    try {
        exec_or_throw(db_, "BEGIN IMMEDIATE;");

        sqlite3_stmt* stmt = nullptr;
        const char* sql = "INSERT INTO messages(room_id, sender_pk, body) VALUES(?, ?, ?);";
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            throw std::runtime_error(sqlite3_errmsg(db_));
        }
        sqlite3_bind_int(stmt, 1, room_id);
        sqlite3_bind_int(stmt, 2, current_user_pk);
        sqlite3_bind_text(stmt, 3, body.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmt) != SQLITE_DONE) {
            finalize(stmt);
            throw std::runtime_error(sqlite3_errmsg(db_));
        }
        sqlite3_finalize(stmt);
        message_id = static_cast<int>(sqlite3_last_insert_rowid(db_));

        sqlite3_stmt* update_stmt = nullptr;
        const char* update_sql = R"SQL(
            UPDATE chat_rooms
            SET last_message = ?,
                last_message_at = (SELECT created_at FROM messages WHERE id = ?)
            WHERE id = ?;
        )SQL";
        if (sqlite3_prepare_v2(db_, update_sql, -1, &update_stmt, nullptr) != SQLITE_OK) {
            throw std::runtime_error(sqlite3_errmsg(db_));
        }
        sqlite3_bind_text(update_stmt, 1, body.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(update_stmt, 2, message_id);
        sqlite3_bind_int(update_stmt, 3, room_id);
        if (sqlite3_step(update_stmt) != SQLITE_DONE) {
            finalize(update_stmt);
            throw std::runtime_error(sqlite3_errmsg(db_));
        }
        sqlite3_finalize(update_stmt);

        exec_or_throw(db_, "COMMIT;");
    } catch (const std::exception& e) {
        sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        error_message = e.what();
        return std::nullopt;
    }

    return message_by_id_locked(message_id);
}
