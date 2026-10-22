#include <pgain/paths.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace std::chrono;
using pgain::Clock;
using pgain::PathMonitor;

namespace {

struct Sim {
    Clock::time_point now = Clock::time_point() + hours(1);
    uint64_t seq = 0;
    std::vector<PathMonitor> paths;

    explicit Sim(size_t n) : paths(n) {}

    void round(const std::vector<milliseconds>& rtt, const std::vector<bool>& drop = {}) {
        for (size_t p = 0; p < paths.size(); p++) {
            paths[p].sent(seq);
            if (!(p < drop.size() && drop[p]))
                paths[p].replied(seq, now, now + rtt[p]);
        }
        seq++;
        now += pgain::probe_interval;
    }
};

}

TEST_CASE("chooses_lowest_rtt_paths") {
    Sim s(3);
    for (int i = 0; i < 20; i++)
        s.round({30ms, 10ms, 20ms});
    CHECK(pgain::choose(s.paths, 1, {}, s.now) == std::vector<size_t>{1});
    CHECK(pgain::choose(s.paths, 2, {}, s.now) == std::vector<size_t>{1, 2});
}

TEST_CASE("srtt_tracks_a_change_in_path_latency") {
    Sim s(1);
    for (int i = 0; i < 20; i++)
        s.round({10ms});
    for (int i = 0; i < 40; i++)
        s.round({50ms});
    CHECK(s.paths[0].srtt() > 45ms);
    CHECK(s.paths[0].srtt() < 55ms);
}

TEST_CASE("silent_path_dies_and_is_dropped") {
    Sim s(2);
    for (int i = 0; i < 20; i++)
        s.round({10ms, 20ms});
    CHECK(pgain::choose(s.paths, 1, {0}, s.now) == std::vector<size_t>{0});
    for (int i = 0; i < 4; i++)
        s.round({10ms, 20ms}, {true, false});
    CHECK_FALSE(s.paths[0].alive(s.now));
    CHECK(pgain::choose(s.paths, 1, {0}, s.now) == std::vector<size_t>{1});
}

TEST_CASE("lossy_path_loses_to_clean_path_of_same_rtt") {
    Sim s(2);
    for (int i = 0; i < 64; i++)
        s.round({10ms, 10ms}, {i % 3 == 0, false});
    CHECK(s.paths[0].loss() > 0.25);
    CHECK(s.paths[1].loss() == 0);
    CHECK(pgain::choose(s.paths, 1, {0}, s.now) == std::vector<size_t>{1});
}

TEST_CASE("active_path_kept_unless_clearly_worse") {
    Sim s(2);
    for (int i = 0; i < 20; i++)
        s.round({21ms, 20ms});
    CHECK(pgain::choose(s.paths, 1, {0}, s.now) == std::vector<size_t>{0});
    for (int i = 0; i < 40; i++)
        s.round({40ms, 20ms});
    CHECK(pgain::choose(s.paths, 1, {0}, s.now) == std::vector<size_t>{1});
}

TEST_CASE("dead_paths_fill_in_when_too_few_are_alive") {
    Sim s(3);
    for (int i = 0; i < 20; i++)
        s.round({10ms, 20ms, 30ms});
    for (int i = 0; i < 4; i++)
        s.round({10ms, 20ms, 30ms}, {true, false, true});
    CHECK(pgain::choose(s.paths, 1, {0}, s.now) == std::vector<size_t>{1});
    CHECK(pgain::choose(s.paths, 3, {0, 1, 2}, s.now).size() == 3);
    CHECK(pgain::choose(s.paths, 3, {0, 1, 2}, s.now)[0] == 1);
}

TEST_CASE("no_live_paths_means_send_on_all") {
    std::vector<PathMonitor> paths(3);
    CHECK(pgain::choose(paths, 1, {}, Clock::now()) == std::vector<size_t>{0, 1, 2});
}

TEST_CASE("stale_or_duplicate_replies_ignored") {
    Sim s(1);
    for (int i = 0; i < 100; i++)
        s.round({10ms});
    auto before = s.paths[0].srtt();
    s.paths[0].replied(s.seq - 1, s.now - seconds(5), s.now);
    s.paths[0].replied(5, s.now - seconds(5), s.now);
    s.paths[0].replied(s.seq + 3, s.now - seconds(5), s.now);
    CHECK(s.paths[0].srtt() == before);
}
