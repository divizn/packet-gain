#include <pgain/auth.hpp>
#include <pgain/udp.hpp>

#include <array>
#include <cstring>
#include <fstream>
#include <openssl/core_names.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <sstream>

namespace pgain {

namespace {

int nibble(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

}

Key parse_key(std::string_view hex) {
    while (!hex.empty() && (hex.back() == '\n' || hex.back() == '\r' || hex.back() == ' '))
        hex.remove_suffix(1);
    if (hex.size() != 2 * key_size)
        throw Error("key must be " + std::to_string(2 * key_size) + " hex characters");
    Key key;
    for (size_t i = 0; i < key_size; i++) {
        int hi = nibble(hex[2 * i]), lo = nibble(hex[2 * i + 1]);
        if (hi < 0 || lo < 0)
            throw Error("key is not hex");
        key[i] = std::byte(hi << 4 | lo);
    }
    return key;
}

Key load_key(const std::string& path) {
    std::ifstream in(path);
    if (!in)
        throw Error("cannot read key file " + path);
    std::stringstream ss;
    ss << in.rdbuf();
    return parse_key(ss.str());
}

std::string generate_key() {
    std::array<unsigned char, key_size> raw;
    if (RAND_bytes(raw.data(), int(raw.size())) != 1)
        throw Error("RAND_bytes failed");
    static constexpr char digits[] = "0123456789abcdef";
    std::string hex;
    for (auto b : raw) {
        hex += digits[b >> 4];
        hex += digits[b & 15];
    }
    return hex;
}

Mac::Mac(const Key& key)
    : mac_(EVP_MAC_fetch(nullptr, "HMAC", nullptr)), ctx_(mac_ ? EVP_MAC_CTX_new(mac_) : nullptr) {
    char digest[] = "SHA256";
    OSSL_PARAM params[] = {OSSL_PARAM_construct_utf8_string(OSSL_MAC_PARAM_DIGEST, digest, 0),
                           OSSL_PARAM_construct_end()};
    if (!ctx_ || !EVP_MAC_init(ctx_, reinterpret_cast<const unsigned char*>(key.data()), key.size(), params)) {
        EVP_MAC_CTX_free(ctx_);
        EVP_MAC_free(mac_);
        throw Error("cannot set up HMAC-SHA256");
    }
}

Mac::~Mac() {
    EVP_MAC_CTX_free(ctx_);
    EVP_MAC_free(mac_);
}

void Mac::sign(std::span<const std::byte> header, std::span<const std::byte> payload,
               std::span<std::byte, tag_size> tag) {
    std::array<unsigned char, EVP_MAX_MD_SIZE> full;
    size_t len = 0;
    if (!EVP_MAC_init(ctx_, nullptr, 0, nullptr) ||
        !EVP_MAC_update(ctx_, reinterpret_cast<const unsigned char*>(header.data()), header.size()) ||
        !EVP_MAC_update(ctx_, reinterpret_cast<const unsigned char*>(payload.data()), payload.size()) ||
        !EVP_MAC_final(ctx_, full.data(), &len, full.size()))
        throw Error("HMAC failed");
    std::memcpy(tag.data(), full.data(), tag_size);
}

bool Mac::verify(std::span<const std::byte> packet) {
    if (packet.size() < tag_size)
        return false;
    std::array<std::byte, tag_size> want;
    auto body = packet.first(packet.size() - tag_size);
    sign(body, {}, want);
    return CRYPTO_memcmp(want.data(), packet.data() + body.size(), tag_size) == 0;
}

}
