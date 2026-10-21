#pragma once

#include <array>
#include <cstddef>
#include <openssl/types.h>
#include <span>
#include <string>
#include <string_view>

namespace pgain {

inline constexpr size_t key_size = 32;
inline constexpr size_t tag_size = 16;

using Key = std::array<std::byte, key_size>;

Key parse_key(std::string_view hex);
Key load_key(const std::string& path);
std::string generate_key();

class Mac {
public:
    explicit Mac(const Key& key);
    Mac(const Mac&) = delete;
    Mac& operator=(const Mac&) = delete;
    ~Mac();

    void sign(std::span<const std::byte> header, std::span<const std::byte> payload,
              std::span<std::byte, tag_size> tag);
    bool verify(std::span<const std::byte> packet);

private:
    EVP_MAC* mac_;
    EVP_MAC_CTX* ctx_;
};

}
