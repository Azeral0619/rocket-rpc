#pragma once

#include <arpa/inet.h>
#include <cstdint>
#include <memory>
#include <netinet/in.h>
#include <string>
#include <string_view>
#include <sys/socket.h>

namespace rocket {

class NetAddr {
  public:
    using s_ptr = std::shared_ptr<NetAddr>;

    virtual ~NetAddr() = default;

    NetAddr(const NetAddr&) = delete;
    NetAddr& operator=(const NetAddr&) = delete;
    NetAddr(NetAddr&&) = delete;
    NetAddr& operator=(NetAddr&&) = delete;

    [[nodiscard]] virtual const sockaddr* getSockAddr() const noexcept = 0;

    [[nodiscard]] virtual socklen_t getSockLen() const noexcept = 0;

    [[nodiscard]] virtual int getFamily() const noexcept = 0;

    [[nodiscard]] virtual std::string toString() const = 0;

    [[nodiscard]] virtual bool checkValid() const = 0;

  protected:
    NetAddr() = default;
};

class IPNetAddr final : public NetAddr {
  public:
    static bool checkValid(std::string_view addr);

    IPNetAddr(std::string_view ip, uint16_t port);

    explicit IPNetAddr(std::string_view addr);

    explicit IPNetAddr(const sockaddr_in& addr);

    [[nodiscard]] const sockaddr* getSockAddr() const noexcept override;

    [[nodiscard]] socklen_t getSockLen() const noexcept override;

    [[nodiscard]] int getFamily() const noexcept override;

    [[nodiscard]] std::string toString() const override;

    [[nodiscard]] bool checkValid() const noexcept override;

  private:
    sockaddr_storage m_addr{};
    socklen_t m_addr_len{sizeof(sockaddr_in)};
};

} // namespace rocket