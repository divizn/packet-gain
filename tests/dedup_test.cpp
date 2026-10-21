#include <pgain/dedup.hpp>

#include <catch2/catch_test_macros.hpp>

using pgain::Dedup;

TEST_CASE("duplicate_rejected") {
    Dedup d;
    CHECK(d.accept(0));
    CHECK_FALSE(d.accept(0));
    CHECK(d.accept(7));
    CHECK_FALSE(d.accept(7));
}

TEST_CASE("out_of_order_within_window_accepted_once") {
    Dedup d;
    CHECK(d.accept(10));
    CHECK(d.accept(4));
    CHECK_FALSE(d.accept(4));
    CHECK(d.accept(10 - Dedup::window + 1));
}

TEST_CASE("older_than_window_rejected") {
    Dedup d;
    CHECK(d.accept(Dedup::window + 5));
    CHECK_FALSE(d.accept(5));
}

TEST_CASE("advancing_clears_slots_reused_by_new_seqs") {
    Dedup d;
    for (uint64_t s = 0; s < Dedup::window; s++)
        CHECK(d.accept(s));
    CHECK(d.accept(Dedup::window + 10));
    for (uint64_t s = Dedup::window; s < Dedup::window + 10; s++)
        CHECK(d.accept(s));
}

TEST_CASE("jump_past_whole_window_clears_everything") {
    Dedup d;
    for (uint64_t s = 0; s < Dedup::window; s++)
        d.accept(s);
    CHECK(d.accept(5 * Dedup::window));
    for (uint64_t s = 4 * Dedup::window + 1; s < 5 * Dedup::window; s++)
        CHECK(d.accept(s));
}
