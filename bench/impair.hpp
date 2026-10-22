#pragma once

#include <pgain/udp.hpp>

#include <chrono>
#include <cstdint>
#include <optional>
#include <queue>
#include <random>
#include <vector>

namespace pgain {

struct Impairment {
    double loss = 0;
    double duplicate = 0;
    std::chrono::microseconds delay{0};
    std::chrono::microseconds jitter{0};
};

class Impairer {
public:
    Impairer(const Address& listen, const Address& target, const Impairment& imp, uint64_t seed);

    void poll(int timeout_ms);
    uint16_t port() const { return local_port(down_); }

private:
    using Clock = std::chrono::steady_clock;

    struct Pending {
        Clock::time_point due;
        bool upward;
        std::vector<std::byte> bytes;
        bool operator>(const Pending& o) const { return due > o.due; }
    };

    void admit(const RecvBatch& rx, size_t n, bool upward);
    void release();

    Impairment imp_;
    std::mt19937_64 rng_;
    Fd down_;
    Fd up_;
    std::optional<Address> peer_;
    std::priority_queue<Pending, std::vector<Pending>, std::greater<>> queue_;
    RecvBatch rx_;
};

}
