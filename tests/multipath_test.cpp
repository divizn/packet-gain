#include <pgain/dedup.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <random>
#include <set>
#include <vector>

namespace {

struct Arrival {
    double at;
    uint64_t seq;
};

struct Oracle {
    std::set<uint64_t> seen;
    uint64_t highest = 0;

    bool accept(uint64_t seq) {
        bool fresh = seen.empty() || seq + pgain::Dedup::window > highest;
        if (!fresh || !seen.insert(seq).second)
            return false;
        highest = std::max(highest, seq);
        return true;
    }
};

std::vector<Arrival> simulate(std::mt19937_64& rng, uint64_t packets, int paths, double interval) {
    std::uniform_real_distribution<double> loss(0, 1), base(1, 40), jitter(0, 30);
    std::vector<Arrival> out;
    for (int p = 0; p < paths; p++) {
        double latency = base(rng);
        double drop = loss(rng) * 0.3;
        for (uint64_t s = 0; s < packets; s++) {
            if (loss(rng) < drop)
                continue;
            double sent = double(s) * interval;
            out.push_back({sent + latency + jitter(rng), s});
            if (loss(rng) < 0.02)
                out.push_back({sent + latency + jitter(rng), s});
        }
    }
    std::sort(out.begin(), out.end(), [](auto& a, auto& b) { return a.at < b.at; });
    return out;
}

}

TEST_CASE("seeded_multipath_runs_deliver_exactly_what_oracle_accepts") {
    for (uint64_t seed = 0; seed < 200; seed++) {
        std::mt19937_64 rng(seed);
        Oracle oracle;
        pgain::Dedup dedup;
        std::vector<int> delivered(5000);
        double interval = seed % 2 ? 1 : 0.01;
        for (auto [at, seq] : simulate(rng, delivered.size(), 1 + int(seed % 3), interval)) {
            bool got = dedup.accept(seq);
            if (got != oracle.accept(seq))
                FAIL("seed " << seed << " seq " << seq);
            delivered[seq] += got;
        }
        for (int n : delivered)
            REQUIRE(n <= 1);
    }
}
