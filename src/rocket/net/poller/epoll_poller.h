#pragma once

// Linux epoll backend for Poller. Compiles to an empty TU on other platforms.

#if defined(__linux__)

#include "rocket/net/poller/poller.h"
#include <unordered_map>

namespace rocket {

class EPollPoller final : public Poller {
  public:
    EPollPoller() = delete;

    // Returns nullptr if epoll_create1 fails.
    [[nodiscard]] static std::unique_ptr<EPollPoller> create();

    ~EPollPoller() override;

    int poll(int timeout_ms, std::vector<ActiveEvent>& out) override;

    void updateFdEvent(FdEvent* event) override;

    void removeFdEvent(FdEvent* event) override;

  private:
    explicit EPollPoller(int epoll_fd);

    [[nodiscard]] static std::uint32_t toEpollEvents(std::uint32_t listen_mask) noexcept;

    int m_epoll_fd{-1};
    // fd -> currently registered epoll event mask, to decide ADD vs MOD.
    std::unordered_map<int, std::uint32_t> m_registered;
};

} // namespace rocket

#endif // __linux__
