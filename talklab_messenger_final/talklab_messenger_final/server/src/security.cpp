#include "security.h"

#include <openssl/evp.h>
#include <openssl/rand.h>

#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace {
std::string bytes_to_hex(const unsigned char* data, std::size_t len) {
    std::ostringstream oss;
    for (std::size_t i = 0; i < len; ++i) {
        oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(data[i]);
    }
    return oss.str();
}
}

namespace security {

std::string random_hex(std::size_t bytes) {
    std::vector<unsigned char> buf(bytes);
    if (RAND_bytes(buf.data(), static_cast<int>(buf.size())) != 1) {
        throw std::runtime_error("RAND_bytes failed");
    }
    return bytes_to_hex(buf.data(), buf.size());
}

std::string pbkdf2_sha256_hex(const std::string& password, const std::string& salt) {
    constexpr int iterations = 120000;
    constexpr int out_len = 32;
    unsigned char out[out_len];
    if (PKCS5_PBKDF2_HMAC(
            password.c_str(), static_cast<int>(password.size()),
            reinterpret_cast<const unsigned char*>(salt.data()), static_cast<int>(salt.size()),
            iterations, EVP_sha256(), out_len, out) != 1) {
        throw std::runtime_error("PBKDF2 failed");
    }
    return bytes_to_hex(out, out_len);
}

bool constant_time_equal(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    unsigned char diff = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        diff |= static_cast<unsigned char>(a[i] ^ b[i]);
    }
    return diff == 0;
}

}
