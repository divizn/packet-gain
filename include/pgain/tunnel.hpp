#pragma once

#include <pgain/dedup.hpp>
#include <pgain/udp.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

namespace pgain {

inline constexpr size_t max_paths = 8;
inline constexpr size_t max_sessions = 4096;
inline constexpr auto session_idle = std::chrono::seconds(60);

class Client {
public:
    Client(const Key& key, const sockaddr_in& listen, const std::vector<sockaddr_in>& paths);

    void poll(int timeout_ms);
    uint16_t port() const { return local_port(app_); }

private:
    void from_app();
    void from_path(size_t p);

    Mac mac_;
    Fd epoll_;
    Fd app_;
    std::vector<Fd> paths_;
    uint32_t session_;
    uint64_t seq_ = 0;
    Dedup dedup_;
    std::optional<sockaddr_in> app_addr_;
    RecvBatch app_rx_{max_payload};
    RecvBatch path_rx_;
    SendBatch tx_{batch_size};
};

class Exit {
public:
    Exit(const Key& key, const sockaddr_in& listen, const sockaddr_in& target);

    void poll(int timeout_ms);
    uint16_t port() const { return local_port(listen_); }
    size_t sessions() const { return sessions_.size(); }

private:
    using Clock = std::chrono::steady_clock;

    struct Path {
        std::optional<sockaddr_in> addr;
        uint64_t newest = 0;
    };

    struct Session {
        uint32_t id;
        Fd upstream;
        Dedup dedup;
        uint64_t seq = 0;
        std::array<Path, max_paths> paths;
        Clock::time_point last_seen;
    };

    Session* session(uint32_t id, Clock::time_point now);
    void from_tunnel();
    void from_upstream(Session& s);
    void expire(Clock::time_point now);

    Mac mac_;
    sockaddr_in target_;
    Fd epoll_;
    Fd listen_;
    std::unordered_map<uint32_t, std::unique_ptr<Session>> sessions_;
    RecvBatch rx_;
    RecvBatch upstream_rx_{max_payload};
    SendBatch tx_{batch_size * max_paths};
    Clock::time_point last_sweep_ = Clock::now();
};

}
