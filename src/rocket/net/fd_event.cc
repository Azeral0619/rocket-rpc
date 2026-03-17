#include "rocket/net/fd_event.h"
#include <cstring>
#include <fcntl.h>
#include <functional>
#include <sys/epoll.h>
#include <unistd.h>
#include <utility>

namespace rocket {

FdEvent::FdEvent(int fd) : m_fd(fd) {
    std::memset(&m_listen_events, 0, sizeof(m_listen_events));
    m_listen_events.data.fd = m_fd;
}

FdEvent::FdEvent() { std::memset(&m_listen_events, 0, sizeof(m_listen_events)); }

bool FdEvent::setNonBlock() {
    if (m_fd < 0) {
        return false;
    }

    const int flags = ::fcntl(m_fd, F_GETFL, 0);
    if (flags == -1) {
        return false;
    }

    const int new_flags = flags | O_NONBLOCK;
    return ::fcntl(m_fd, F_SETFL, new_flags) != -1;
}

const std::function<void()>& FdEvent::handler(TriggerEvent event_type) const {
    switch (event_type) {
    case TriggerEvent::IN_EVENT:
        return m_read_callback;
    case TriggerEvent::OUT_EVENT:
        return m_write_callback;
    case TriggerEvent::ERROR_EVENT:
    default:
        return m_error_callback;
    }
}

void FdEvent::listen(TriggerEvent event_type, std::function<void()> callback) {
    switch (event_type) {
    case TriggerEvent::IN_EVENT:
        m_read_callback = std::move(callback);
        m_listen_events.events |= EPOLLIN;
        break;
    case TriggerEvent::OUT_EVENT:
        m_write_callback = std::move(callback);
        m_listen_events.events |= EPOLLOUT;
        break;
    case TriggerEvent::ERROR_EVENT:
        m_error_callback = std::move(callback);
        m_listen_events.events |= EPOLLERR;
        break;
    }
}

void FdEvent::setErrorCallback(std::function<void()> callback) {
    m_error_callback = std::move(callback);
    m_listen_events.events |= EPOLLERR;
}

void FdEvent::cancel(TriggerEvent event_type) {
    switch (event_type) {
    case TriggerEvent::IN_EVENT:
        m_read_callback = nullptr;
        m_listen_events.events &= ~EPOLLIN;
        break;
    case TriggerEvent::OUT_EVENT:
        m_write_callback = nullptr;
        m_listen_events.events &= ~EPOLLOUT;
        break;
    case TriggerEvent::ERROR_EVENT:
        m_error_callback = nullptr;
        m_listen_events.events &= ~EPOLLERR;
        break;
    }
}

void FdEvent::clearCallbacks() noexcept {
    m_read_callback = nullptr;
    m_write_callback = nullptr;
    m_error_callback = nullptr;
    m_listen_events.events = 0;
}

bool FdEvent::isListening(TriggerEvent event_type) const noexcept {
    switch (event_type) {
    case TriggerEvent::IN_EVENT:
        return (m_listen_events.events & EPOLLIN) != 0;
    case TriggerEvent::OUT_EVENT:
        return (m_listen_events.events & EPOLLOUT) != 0;
    case TriggerEvent::ERROR_EVENT:
        return (m_listen_events.events & EPOLLERR) != 0;
    default:
        return false;
    }
}

void FdEvent::updateEpollEvents() {
    m_listen_events.events = 0;
    if (m_read_callback) {
        m_listen_events.events |= EPOLLIN;
    }
    if (m_write_callback) {
        m_listen_events.events |= EPOLLOUT;
    }
    if (m_error_callback) {
        m_listen_events.events |= EPOLLERR;
    }
}

} // namespace rocket
