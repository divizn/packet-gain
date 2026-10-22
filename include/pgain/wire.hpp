#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace pgain {

inline constexpr uint8_t wire_version = 1;
inline constexpr size_t header_size = 16;

enum class Type : uint8_t {
    data = 1,
    probe = 2,
    probe_reply = 3,
};

struct Header {
    Type type;
    uint8_t path;
    uint32_t session;
    uint64_t seq;
};

void encode(const Header& h, std::span<std::byte, header_size> out);
std::optional<Header> decode(std::span<const std::byte> packet);

}
