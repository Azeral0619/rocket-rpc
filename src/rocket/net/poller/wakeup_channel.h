#pragma once

// Cross-platform loop wakeup channel: eventfd on Linux, pipe on macOS.
// The read end is registered in the Poller; wakeup() writes to the write end.

#include <memory>

namespace rocket {

class WakeupChannel {
  public:
    // Returns nullptr on failure. Read and write ends are non-blocking + CLOEXEC.
    [[nodiscard]] static std::unique_ptr<WakeupChannel> create();

    virtual ~WakeupChannel() = default;

    WakeupChannel(const WakeupChannel&) = delete;
    WakeupChannel& operator=(const WakeupChannel&) = delete;
    WakeupChannel(WakeupChannel&&) = delete;
    WakeupChannel& operator=(WakeupChannel&&) = delete;

    // Fd to register for IN events in the poller.
    [[nodiscard]] virtual bool isValid() const noexcept = 0;

    [[nodiscard]] virtual int readFd() const noexcept = 0;

    // Wake the loop (any thread, async-signal-safe-ish).
    virtual void wakeup() = 0;

    // Drain pending wakeup bytes; call from the loop thread when the fd fires.
    virtual void drain() = 0;

  protected:
    WakeupChannel() = default;
};

} // namespace rocket
