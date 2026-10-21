#include <pgain/auth.hpp>
#include <pgain/udp.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_CASE("parse_key_rejects_wrong_length_and_non_hex") {
    CHECK_NOTHROW(pgain::parse_key(std::string(64, 'a') + "\n"));
    CHECK_THROWS_AS(pgain::parse_key(std::string(63, 'a')), pgain::Error);
    CHECK_THROWS_AS(pgain::parse_key(std::string(66, 'a')), pgain::Error);
    CHECK_THROWS_AS(pgain::parse_key(std::string(63, 'a') + "g"), pgain::Error);
}

TEST_CASE("generated_keys_parse_and_differ") {
    auto a = pgain::generate_key(), b = pgain::generate_key();
    CHECK(a != b);
    CHECK_NOTHROW(pgain::parse_key(a));
}

TEST_CASE("tag_depends_on_key") {
    std::array<std::byte, 4> body{};
    std::array<std::byte, pgain::tag_size> t1, t2;
    pgain::Mac(pgain::parse_key(std::string(64, '1'))).sign(body, {}, t1);
    pgain::Mac(pgain::parse_key(std::string(64, '2'))).sign(body, {}, t2);
    CHECK(t1 != t2);
}
