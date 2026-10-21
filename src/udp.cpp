#include <pgain/udp.hpp>

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netdb.h>
#include <unistd.h>
#include <utility>

namespace pgain {

namespace {

[[noreturn]] void fail(const std::string& what, int err = errno) {
    throw Error(what + ": " + std::strerror(err));
}

Fd udp_socket(int family) {
    Fd fd(socket(family, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
    if (fd.get() < 0)
        fail("socket");
    return fd;
}

}

Address parse_address(std::string_view host_port) {
    auto bad = [&] { return Error("expected host:port or [v6]:port, got " + std::string(host_port)); };
    std::string_view host, port;
    if (host_port.starts_with('[')) {
        auto close = host_port.find("]:");
        if (close == std::string_view::npos)
            throw bad();
        host = host_port.substr(1, close - 1);
        port = host_port.substr(close + 2);
    } else {
        auto colon = host_port.find(':');
        if (colon == std::string_view::npos || host_port.find(':', colon + 1) != std::string_view::npos)
            throw bad();
        host = host_port.substr(0, colon);
        port = host_port.substr(colon + 1);
    }
    if (host.empty() || port.empty())
        throw bad();
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* res = nullptr;
    if (int rc = getaddrinfo(std::string(host).c_str(), std::string(port).c_str(), &hints, &res); rc != 0)
        throw Error("resolve " + std::string(host_port) + ": " + gai_strerror(rc));
    Address addr;
    std::memcpy(&addr.storage, res->ai_addr, res->ai_addrlen);
    addr.len = res->ai_addrlen;
    freeaddrinfo(res);
    return addr;
}

Fd& Fd::operator=(Fd&& other) noexcept {
    if (this != &other) {
        if (fd_ >= 0)
            close(fd_);
        fd_ = std::exchange(other.fd_, -1);
    }
    return *this;
}

Fd::~Fd() {
    if (fd_ >= 0)
        close(fd_);
}

Fd udp_bind(const Address& addr) {
    Fd fd = udp_socket(addr.storage.ss_family);
    int v6only = 0;
    if (addr.storage.ss_family == AF_INET6 &&
        setsockopt(fd.get(), IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof(v6only)) < 0)
        fail("setsockopt IPV6_V6ONLY");
    if (bind(fd.get(), addr.get(), addr.len) < 0)
        fail("bind");
    return fd;
}

Fd udp_connect(const Address& addr) {
    Fd fd = udp_socket(addr.storage.ss_family);
    if (connect(fd.get(), addr.get(), addr.len) < 0)
        fail("connect");
    return fd;
}

uint16_t local_port(const Fd& fd) {
    sockaddr_storage addr{};
    socklen_t len = sizeof(addr);
    if (getsockname(fd.get(), reinterpret_cast<sockaddr*>(&addr), &len) < 0)
        fail("getsockname");
    if (addr.ss_family == AF_INET6)
        return ntohs(reinterpret_cast<const sockaddr_in6&>(addr).sin6_port);
    return ntohs(reinterpret_cast<const sockaddr_in&>(addr).sin_port);
}

RecvBatch::RecvBatch(size_t limit) : limit_(limit), bufs_(batch_size) {}

size_t RecvBatch::recv(const Fd& fd) {
    for (size_t i = 0; i < batch_size; i++) {
        iovs_[i] = {bufs_[i].data(), limit_};
        msgs_[i] = {};
        msgs_[i].msg_hdr.msg_name = &names_[i].storage;
        msgs_[i].msg_hdr.msg_namelen = sizeof(sockaddr_storage);
        msgs_[i].msg_hdr.msg_iov = &iovs_[i];
        msgs_[i].msg_hdr.msg_iovlen = 1;
    }
    for (;;) {
        int n = recvmmsg(fd.get(), msgs_.data(), batch_size, 0, nullptr);
        if (n >= 0) {
            for (int i = 0; i < n; i++) {
                names_[i].len = msgs_[i].msg_hdr.msg_namelen;
                if (msgs_[i].msg_hdr.msg_flags & MSG_TRUNC)
                    msgs_[i].msg_len = 0;
            }
            return size_t(n);
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return 0;
        if (errno != EINTR && errno != ECONNREFUSED && errno != EHOSTUNREACH && errno != ENETUNREACH)
            fail("recvmmsg");
    }
}

SendBatch::SendBatch(size_t capacity) : headers_(capacity), tags_(capacity), iovs_(capacity), msgs_(capacity) {}

void SendBatch::add(const Address* to, std::span<const std::byte> payload) {
    if (n_ == msgs_.size())
        throw Error("send batch full");
    iovs_[n_][0] = {const_cast<std::byte*>(payload.data()), payload.size()};
    msgs_[n_] = {};
    msgs_[n_].msg_hdr.msg_name = to ? const_cast<sockaddr_storage*>(&to->storage) : nullptr;
    msgs_[n_].msg_hdr.msg_namelen = to ? to->len : 0;
    msgs_[n_].msg_hdr.msg_iov = iovs_[n_].data();
    msgs_[n_].msg_hdr.msg_iovlen = 1;
    n_++;
}

void SendBatch::add(const Address* to, const Header& h, std::span<const std::byte> payload, Mac& mac) {
    add(to, payload);
    size_t i = n_ - 1;
    encode(h, headers_[i]);
    mac.sign(headers_[i], payload, tags_[i]);
    iovs_[i][1] = iovs_[i][0];
    iovs_[i][0] = {headers_[i].data(), header_size};
    iovs_[i][2] = {tags_[i].data(), tag_size};
    msgs_[i].msg_hdr.msg_iovlen = 3;
}

void SendBatch::flush(const Fd& fd) {
    size_t sent = 0;
    while (sent < n_) {
        int n = sendmmsg(fd.get(), msgs_.data() + sent, unsigned(n_ - sent), 0);
        if (n > 0)
            sent += size_t(n);
        else if (errno == EAGAIN || errno == EWOULDBLOCK)
            break;
        else if (errno != EINTR)
            sent++;
    }
    n_ = 0;
}

}
