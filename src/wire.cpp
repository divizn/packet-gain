#include <pgain/wire.hpp>

namespace pgain {

namespace {

template <class T>
void put_be(std::byte* p, T v) {
    for (size_t i = 0; i < sizeof(T); i++)
        p[i] = std::byte(v >> (8 * (sizeof(T) - 1 - i)));
}

template <class T>
T get_be(const std::byte* p) {
    T v = 0;
    for (size_t i = 0; i < sizeof(T); i++)
        v = T(v << 8) | T(p[i]);
    return v;
}

}

void encode(const Header& h, std::span<std::byte, header_size> out) {
    out[0] = std::byte(wire_version);
    out[1] = std::byte(h.type);
    out[2] = std::byte(h.path);
    out[3] = std::byte(0);
    put_be(&out[4], h.session);
    put_be(&out[8], h.seq);
}

std::optional<Header> decode(std::span<const std::byte> packet) {
    if (packet.size() < header_size || uint8_t(packet[0]) != wire_version)
        return std::nullopt;
    auto type = Type(packet[1]);
    if (type != Type::data && type != Type::probe && type != Type::probe_reply)
        return std::nullopt;
    return Header{type, uint8_t(packet[2]), get_be<uint32_t>(&packet[4]), get_be<uint64_t>(&packet[8])};
}

}
