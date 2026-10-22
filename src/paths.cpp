#include <pgain/paths.hpp>

#include <algorithm>
#include <numeric>

namespace pgain {

namespace {

double ms(Clock::duration d) { return std::chrono::duration<double, std::milli>(d).count(); }

}

void PathMonitor::sent(uint64_t seq) {
    replied_[seq % window] = false;
    next_ = seq + 1;
}

void PathMonitor::replied(uint64_t seq, Clock::time_point sent_at, Clock::time_point now) {
    if (seq >= next_ || next_ - seq > window || replied_[seq % window] || sent_at > now)
        return;
    replied_[seq % window] = true;
    auto rtt = now - sent_at;
    if (!measured_) {
        srtt_ = rtt;
        rttvar_ = rtt / 2;
        measured_ = true;
    } else {
        auto err = srtt_ > rtt ? srtt_ - rtt : rtt - srtt_;
        rttvar_ = (3 * rttvar_ + err) / 4;
        srtt_ = (7 * srtt_ + rtt) / 8;
    }
    last_reply_ = std::max(last_reply_, now);
}

bool PathMonitor::alive(Clock::time_point now) const {
    return measured_ && now - last_reply_ < 3 * probe_interval + 4 * rttvar_;
}

double PathMonitor::loss() const {
    if (next_ <= in_flight)
        return 0;
    uint64_t end = next_ - in_flight, begin = next_ > window ? next_ - window : 0;
    uint64_t missing = 0;
    for (uint64_t s = begin; s < end; s++)
        missing += !replied_[s % window];
    return double(missing) / double(end - begin);
}

double PathMonitor::score_ms() const { return ms(srtt_) + 2 * ms(rttvar_) + 200 * loss(); }

std::vector<size_t> choose(const std::vector<PathMonitor>& paths, size_t k, const std::vector<size_t>& active,
                           Clock::time_point now) {
    std::vector<size_t> order(paths.size());
    std::iota(order.begin(), order.end(), 0);
    if (std::none_of(paths.begin(), paths.end(), [&](auto& p) { return p.alive(now); }))
        return order;
    auto score = [&](size_t i) {
        bool current = std::find(active.begin(), active.end(), i) != active.end();
        return current ? paths[i].score_ms() * 0.9 - 1 : paths[i].score_ms();
    };
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        bool la = paths[a].alive(now), lb = paths[b].alive(now);
        return la != lb ? la : la && score(a) < score(b);
    });
    order.resize(std::min(k, order.size()));
    return order;
}

}
