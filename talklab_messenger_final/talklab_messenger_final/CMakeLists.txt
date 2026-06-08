cmake_minimum_required(VERSION 3.16)
project(talklab_messenger_final LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)

add_executable(talk_server
    server/src/main.cpp
    server/src/database.cpp
    server/src/security.cpp
)

target_include_directories(talk_server PRIVATE server/src)
target_compile_definitions(talk_server PRIVATE BOOST_ERROR_CODE_HEADER_ONLY)
target_link_libraries(talk_server PRIVATE
    OpenSSL::SSL
    OpenSSL::Crypto
    Threads::Threads
    sqlite3
)
