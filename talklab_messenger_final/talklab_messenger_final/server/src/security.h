#pragma once

#include <cstddef>
#include <string>

namespace security {
std::string random_hex(std::size_t bytes);
std::string pbkdf2_sha256_hex(const std::string& password, const std::string& salt);
bool constant_time_equal(const std::string& a, const std::string& b);
}
