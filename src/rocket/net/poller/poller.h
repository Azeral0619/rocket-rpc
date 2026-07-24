#pragma once

#include "rocket/net/fd_event.h"
#include <cstdint>
#include <memory>
#include <vector>

namespace rocket {

// Platform-neutral IO multiplexing interface (muduo-style).
// Implemented by EPollPoller (Linux) and KqueuePoller (macOS);
// created via Poller::createDefault() which is the only place
// with platform #ifdef.
class Poller {
  public:
    // One ready fd: the FdEvent plus a bitmask of TriggerEvent that fired.
    struct ActiveEvent {
        FdEvent* fd_event{nullptr};
        std::uint32_t event_mask{0};
    };

    static constexpr int kInfiniteTimeout = -1;

    virtual ~Poller() = default;

    Poller(const Poller&) = delete;
    Poller& operator=(const Poller&) = delete;
    Poller(Poller&&) = delete;
    Poller& operator=(Poller&&) = delete;

    // Wait up to timeout_ms (kInfiniteTimeout = block forever).
    // Fills `out` with ready events (cleared first).
    // Returns the number of ready events, or -1 on fatal error (e.g. EBADF).
    virtual int poll(int timeout_ms, std::vector<ActiveEvent>& out) = 0;

    // Register the fd or update the listened event set.
    virtual void updateFdEvent(FdEvent* event) = 0;

    // Unregister the fd (no-op if not registered).
    virtual void removeFdEvent(FdEvent* event) = 0;

    // Create the platform default backend (epoll on Linux, kqueue on macOS).
    // Returns nullptr on failure.
    [[nodiscard]] static std::unique_ptr<Poller> createDefault();

  protected:
    Poller() = default;
};

} // namespace rocket
