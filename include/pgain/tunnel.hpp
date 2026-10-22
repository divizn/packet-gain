#pragma once

#include <pgain/dedup.hpp>
#include <pgain/paths.hpp>
#include <pgain/udp.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace pgain {

inline constexpr size_t max_paths = 8;
inline constexpr size_t max_sessions = 4096;
inline constexpr auto session_idle = std::chrono::seconds(60);
inline constexpr auto path_idle = std::chrono::seconds(1);

class Client {
public:
    Client(const Key& key, const Address& listen, const std::vector<Address>& paths, size_t send_paths = 2);

    void poll(int timeout_ms);
    uint16_t port() const { return local_port(app_); }
    const std::vector<size_t>& active() const { return active_; }

private:
    void from_app();
    void from_path(size_t p);
    void probe(Clock::time_point now);

    Mac mac_;
    Fd epoll_;
    Fd app_;
    std::vector<Fd> paths_;
    uint32_t session_;
    uint64_t seq_ = 0;
    Dedup dedup_;
    std::optional<Address> app_addr_;
    size_t send_paths_;
    std::vector<PathMonitor> monitors_;
    std::vector<size_t> active_;
    uint64_t probe_seq_ = 0;
    Clock::time_point next_probe_ = Clock::now();
    RecvBatch app_rx_{max_payload};
    RecvBatch path_rx_;
    SendBatch tx_{batch_size};
};

class Exit {
public:
    Exit(const Key& key, const Address& listen, const Address& target);

    void poll(int timeout_ms);
    uint16_t port() const { return local_port(listen_); }
    size_t sessions() const { return sessions_.size(); }

private:
    struct Path {
        std::optional<Address> addr;
        uint64_t newest = 0;
        Clock::time_point seen;
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
    Address target_;
    Fd epoll_;
    Fd listen_;
    std::unordered_map<uint32_t, std::unique_ptr<Session>> sessions_;
    RecvBatch rx_;
    RecvBatch upstream_rx_{max_payload};
    SendBatch tx_{batch_size * max_paths};
    SendBatch upstream_tx_{batch_size};
    Clock::time_point last_sweep_ = Clock::now();
};

class Relay {
public:
    Relay(const Key& key, const Address& listen, const Address& next_hop);

    void poll(int timeout_ms);
    uint16_t port() const { return local_port(listen_); }
    size_t flows() const { return flows_.size(); }

private:
    struct Flow {
        Address from;
        Fd upstream;
        Clock::time_point last_seen;
    };

    Flow* flow(const Address& from, Clock::time_point now);
    void from_down();
    void from_up(Flow& f);

    Mac mac_;
    Address next_hop_;
    Fd epoll_;
    Fd listen_;
    std::unordered_map<std::string, std::unique_ptr<Flow>> flows_;
    RecvBatch rx_;
    SendBatch tx_{batch_size};
    Clock::time_point last_sweep_ = Clock::now();
};

}
