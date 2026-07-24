#pragma once

#include <cstdint>
#include <fcntl.h>
#include <functional>

namespace rocket {

class FdEvent {
  public:
    // Platform-independent trigger event values (NOT epoll flags).
    enum class TriggerEvent : std::uint8_t {
        IN_EVENT = 1,
        OUT_EVENT = 2,
        ERROR_EVENT = 3,
    };

    explicit FdEvent(int fd);

    FdEvent();

    ~FdEvent() = default;

    FdEvent(const FdEvent&) = delete;
    FdEvent& operator=(const FdEvent&) = delete;
    FdEvent(FdEvent&&) noexcept = default;
    FdEvent& operator=(FdEvent&&) noexcept = default;

    bool setNonBlock();

    [[nodiscard]] const std::function<void()>& handler(TriggerEvent event_type) const;

    void listen(TriggerEvent event_type, std::function<void()> callback);

    void setErrorCallback(std::function<void()> callback);

    void cancel(TriggerEvent event_type);

    void clearCallbacks() noexcept;

    [[nodiscard]] int getFd() const noexcept { return m_fd; }

    [[nodiscard]] bool isListening(TriggerEvent event_type) const noexcept;

    [[nodiscard]] bool isValid() const noexcept { return m_fd >= 0; }

    // Returns a platform-independent bitmask of TriggerEvent values.
    [[nodiscard]] std::uint32_t listenMask() const noexcept;

  protected:
    int m_fd{-1};
    std::function<void()> m_read_callback;
    std::function<void()> m_write_callback;
    std::function<void()> m_error_callback;
};

// Convert TriggerEvent to a bitmask bit (platform-independent).
[[nodiscard]] constexpr std::uint32_t toMask(FdEvent::TriggerEvent e) noexcept {
    switch (e) {
    case FdEvent::TriggerEvent::IN_EVENT:
        return 1U << 0;
    case FdEvent::TriggerEvent::OUT_EVENT:
        return 1U << 1;
    case FdEvent::TriggerEvent::ERROR_EVENT:
        return 1U << 2;
    }
    return 0;
}

} // namespace rocket
