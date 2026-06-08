#pragma once

#include <sqlite3.h>

#include <mutex>
#include <optional>
#include <string>
#include <vector>

struct UserInfo {
    int id{};
    std::string user_id;
    std::string display_name;
};

struct FriendInfo {
    int id{};
    std::string user_id;
    std::string display_name;
    std::string created_at;
};

struct RoomInfo {
    int id{};
    bool is_direct{};
    std::string title;
    std::string display_title;
    std::string last_message;
    std::string last_message_at;
    std::string created_at;
    int member_count{};
};

struct MessageInfo {
    int id{};
    int room_id{};
    int sender_pk{};
    std::string sender_user_id;
    std::string sender_display_name;
    std::string body;
    std::string created_at;
};

class Database {
public:
    explicit Database(const std::string& path);
    ~Database();

    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    void init();

    bool create_user(const std::string& user_id,
                     const std::string& display_name,
                     const std::string& password,
                     std::string& error_message);

    std::optional<UserInfo> verify_login(const std::string& user_id,
                                         const std::string& password);

    std::string create_session(int user_pk);
    std::optional<UserInfo> user_by_token(const std::string& token);
    void delete_session(const std::string& token);

    std::vector<UserInfo> search_users(int current_user_pk, const std::string& query, int limit = 20);
    bool add_friend(int current_user_pk, const std::string& friend_user_id, std::string& error_message);
    std::vector<FriendInfo> list_friends(int current_user_pk);

    std::optional<RoomInfo> create_direct_room(int current_user_pk,
                                               const std::string& friend_user_id,
                                               std::string& error_message);
    std::vector<RoomInfo> list_rooms(int current_user_pk);
    std::vector<MessageInfo> list_messages(int current_user_pk, int room_id, int limit = 80);
    std::optional<MessageInfo> create_message(int current_user_pk,
                                              int room_id,
                                              const std::string& body,
                                              std::string& error_message);
    std::vector<int> room_member_ids(int room_id);
    bool is_room_member(int user_pk, int room_id);

private:
    sqlite3* db_ = nullptr;
    std::mutex mutex_;

    std::optional<UserInfo> user_by_user_id_locked(const std::string& user_id);
    bool is_room_member_locked(int user_pk, int room_id);
    std::optional<RoomInfo> room_by_id_for_user_locked(int current_user_pk, int room_id);
    std::optional<MessageInfo> message_by_id_locked(int message_id);
};
