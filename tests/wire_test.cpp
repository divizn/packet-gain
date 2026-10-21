#include <pgain/wire.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>

namespace {

template <class... T>
std::array<std::byte, sizeof...(T)> bytes(T... v) {
    return {std::byte(v)...};
}

}

TEST_CASE("encode_matches_wire_layout") {
    std::array<std::byte, pgain::header_size> out;
    pgain::encode({pgain::Type::data, 3, 0x01020304, 0x05060708090a0b0c}, out);
    CHECK(out == bytes(1, 1, 3, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12));
}

TEST_CASE("decode_reads_header_and_ignores_payload") {
    auto in = bytes(1, 1, 3, 0, 0xff, 0, 0, 1, 0x80, 0, 0, 0, 0, 0, 0, 2, 0xaa, 0xbb);
    auto h = pgain::decode(in);
    REQUIRE(h);
    CHECK(h->type == pgain::Type::data);
    CHECK(h->path == 3);
    CHECK(h->session == 0xff000001);
    CHECK(h->seq == 0x8000000000000002);
}

TEST_CASE("decode_rejects_bad_packets") {
    CHECK_FALSE(pgain::decode(bytes(1, 1, 3, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0)));
    CHECK_FALSE(pgain::decode(bytes(2, 1, 3, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 2)));
    CHECK_FALSE(pgain::decode(bytes(1, 9, 3, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 2)));
}
