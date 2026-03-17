#include "net_addr.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <array>
#include <charconv>
#include <cstdint>
#include <cstring>
#include <fmt/format.h>
#include <string>
#include <string_view>
#include <system_error>
namespace rocket {

constexpr std::uint16_t kMaxPort = 65535;

namespace {
inline sockaddr_in* as_sockaddr_in(sockaddr_storage* storage) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    return reinterpret_cast<sockaddr_in*>(storage);
}

inline const sockaddr_in* as_sockaddr_in(const sockaddr_storage* storage) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    return reinterpret_cast<const sockaddr_in*>(storage);
}

inline const sockaddr* as_sockaddr(const sockaddr_storage* storage) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    return reinterpret_cast<const sockaddr*>(storage);
}

bool parse_ipv4(std::string_view ip_view, in_addr& out) {
    std::string ip_str(ip_view);
    return inet_pton(AF_INET, ip_str.c_str(), &out) == 1;
}
} // namespace

bool IPNetAddr::checkValid(std::string_view addr) {
    size_t colon_pos = addr.find(':');
    if (colon_pos == std::string_view::npos) {
        return false;
    }

    std::string_view ip = addr.substr(0, colon_pos);
    std::string_view port_str = addr.substr(colon_pos + 1);

    if (ip.empty() || port_str.empty()) {
        return false;
    }

    int port = 0;
    auto [ptr, ec] = std::from_chars(port_str.data(), port_str.data() + port_str.size(), port);
    if (ec != std::errc{} || port <= 0 || port > kMaxPort) {
        return false;
    }

    in_addr dummy{};
    return parse_ipv4(ip, dummy);
}

IPNetAddr::IPNetAddr(std::string_view ip, uint16_t port) {
    sockaddr_in* addr_in = as_sockaddr_in(&m_addr);
    addr_in->sin_family = AF_INET;
    addr_in->sin_port = htons(port);

    if (!parse_ipv4(ip, addr_in->sin_addr)) {
        addr_in->sin_addr.s_addr = INADDR_ANY;
    }

    m_addr_len = sizeof(sockaddr_in);
}

IPNetAddr::IPNetAddr(std::string_view addr) {
    size_t colon_pos = addr.find(':');
    if (colon_pos == std::string_view::npos) {
        sockaddr_in* addr_in = as_sockaddr_in(&m_addr);
        addr_in->sin_family = AF_INET;
        addr_in->sin_port = 0;
        addr_in->sin_addr.s_addr = INADDR_ANY;
        m_addr_len = sizeof(sockaddr_in);
        return;
    }

    std::string_view ip = addr.substr(0, colon_pos);
    std::string_view port_str = addr.substr(colon_pos + 1);

    uint16_t port = 0;
    std::from_chars(port_str.data(), port_str.data() + port_str.size(), port);

    sockaddr_in* addr_in = as_sockaddr_in(&m_addr);
    addr_in->sin_family = AF_INET;
    addr_in->sin_port = htons(port);

    if (!parse_ipv4(ip, addr_in->sin_addr)) {
        addr_in->sin_addr.s_addr = INADDR_ANY;
    }

    m_addr_len = sizeof(sockaddr_in);
}

IPNetAddr::IPNetAddr(const sockaddr_in& addr) : m_addr_len(sizeof(sockaddr_in)) {
    std::memcpy(&m_addr, &addr, sizeof(sockaddr_in));
}

const sockaddr* IPNetAddr::getSockAddr() const noexcept { return as_sockaddr(&m_addr); }

socklen_t IPNetAddr::getSockLen() const noexcept { return m_addr_len; }

int IPNetAddr::getFamily() const noexcept { return as_sockaddr(&m_addr)->sa_family; }

std::string IPNetAddr::toString() const {
    const sockaddr_in* addr_in = as_sockaddr_in(&m_addr);

    std::array<char, INET_ADDRSTRLEN> ip_buf{};
    if (inet_ntop(AF_INET, &addr_in->sin_addr, ip_buf.data(), ip_buf.size()) == nullptr) {
        return "invalid";
    }

    return fmt::format("{}:{}", ip_buf.data(), ntohs(addr_in->sin_port));
}

bool IPNetAddr::checkValid() const noexcept {
    const sockaddr_in* addr_in = as_sockaddr_in(&m_addr);
    return addr_in->sin_family == AF_INET && addr_in->sin_port != 0;
}

} // namespace rocket