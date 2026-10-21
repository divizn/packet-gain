#include <pgain/udp.hpp>

#include <catch2/catch_test_macros.hpp>

#include <netinet/in.h>

TEST_CASE("parse_address_accepts_v4_and_bracketed_v6") {
    auto v4 = pgain::parse_address("127.0.0.1:7000");
    CHECK(v4.storage.ss_family == AF_INET);
    CHECK(v4.len == sizeof(sockaddr_in));
    auto v6 = pgain::parse_address("[::1]:7000");
    CHECK(v6.storage.ss_family == AF_INET6);
    CHECK(v6.len == sizeof(sockaddr_in6));
    CHECK(ntohs(reinterpret_cast<const sockaddr_in6&>(v6.storage).sin6_port) == 7000);
}

TEST_CASE("parse_address_rejects_ambiguous_or_incomplete") {
    for (auto bad : {"::1:7000", "[::1]7000", "[::1]", "127.0.0.1", "127.0.0.1:", ":7000", "[]:7000"})
        CHECK_THROWS_AS(pgain::parse_address(bad), pgain::Error);
}
