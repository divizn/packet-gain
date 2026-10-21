#pragma once

#include <pgain/wire.hpp>

#include <array>
#include <cstddef>
#include <memory>
#include <netinet/in.h>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <utility>
#include <vector>

namespace pgain {

struct Error : std::runtime_error {
    using std::runtime_error::runtime_error;
};

sockaddr_in parse_address(std::string_view host_port);

class Fd {
public:
    explicit Fd(int fd = -1) : fd_(fd) {}
    Fd(Fd&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}
    Fd& operator=(Fd&& other) noexcept;
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    ~Fd();

    int get() const { return fd_; }

private:
    int fd_;
};

Fd udp_bind(const sockaddr_in& addr);
Fd udp_connect(const sockaddr_in& addr);
uint16_t local_port(const Fd& fd);

inline constexpr size_t batch_size = 32;
inline constexpr size_t max_packet = 2048;

class RecvBatch {
public:
    explicit RecvBatch(size_t limit = max_packet);

    size_t recv(const Fd& fd);
    std::span<const std::byte> packet(size_t i) const { return {bufs_[i].data(), msgs_[i].msg_len}; }
    const sockaddr_in& from(size_t i) const { return names_[i]; }

private:
    size_t limit_;
    std::vector<std::array<std::byte, max_packet>> bufs_;
    std::array<sockaddr_in, batch_size> names_;
    std::array<iovec, batch_size> iovs_;
    std::array<mmsghdr, batch_size> msgs_;
};

class SendBatch {
public:
    explicit SendBatch(size_t capacity);

    void add(const sockaddr_in* to, std::span<const std::byte> payload);
    void add(const sockaddr_in* to, const Header& h, std::span<const std::byte> payload);
    void flush(const Fd& fd);

private:
    std::vector<std::array<std::byte, header_size>> headers_;
    std::vector<std::array<iovec, 2>> iovs_;
    std::vector<mmsghdr> msgs_;
    size_t n_ = 0;
};

}
