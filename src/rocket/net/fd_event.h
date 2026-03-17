#pragma once

#include <cstdint>
#include <fcntl.h>
#include <functional>
#include <sys/epoll.h>

namespace rocket {

class FdEvent {
  public:
    enum class TriggerEvent : std::uint8_t {
        IN_EVENT = EPOLLIN,
        OUT_EVENT = EPOLLOUT,
        ERROR_EVENT = EPOLLERR,
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

    [[nodiscard]] const epoll_event& getEpollEvent() const noexcept { return m_listen_events; }

    [[nodiscard]] epoll_event& getEpollEvent() noexcept { return m_listen_events; }

    [[nodiscard]] bool isListening(TriggerEvent event_type) const noexcept;

    [[nodiscard]] bool isValid() const noexcept { return m_fd >= 0; }

    void updateEpollEvents();

  protected:
    int m_fd{-1};
    epoll_event m_listen_events{};
    std::function<void()> m_read_callback;
    std::function<void()> m_write_callback;
    std::function<void()> m_error_callback;
};

} // namespace rocket