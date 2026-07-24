#include "rocket/net/fd_event.h"
#include <fcntl.h>
#include <unistd.h>
#include <utility>

namespace rocket {

FdEvent::FdEvent(int fd) : m_fd(fd) {}

FdEvent::FdEvent() {}

bool FdEvent::setNonBlock() {
    if (m_fd < 0) return false;
    const int flags = ::fcntl(m_fd, F_GETFL, 0);
    if (flags == -1) return false;
    return ::fcntl(m_fd, F_SETFL, flags | O_NONBLOCK) != -1;
}

const std::function<void()>& FdEvent::handler(TriggerEvent event_type) const {
    switch (event_type) {
    case TriggerEvent::IN_EVENT:   return m_read_callback;
    case TriggerEvent::OUT_EVENT:  return m_write_callback;
    case TriggerEvent::ERROR_EVENT:
    default:                       return m_error_callback;
    }
}

void FdEvent::listen(TriggerEvent event_type, std::function<void()> callback) {
    switch (event_type) {
    case TriggerEvent::IN_EVENT:    m_read_callback = std::move(callback); break;
    case TriggerEvent::OUT_EVENT:   m_write_callback = std::move(callback); break;
    case TriggerEvent::ERROR_EVENT: m_error_callback = std::move(callback); break;
    }
}

void FdEvent::setErrorCallback(std::function<void()> callback) {
    m_error_callback = std::move(callback);
}

void FdEvent::cancel(TriggerEvent event_type) {
    switch (event_type) {
    case TriggerEvent::IN_EVENT:    m_read_callback = nullptr; break;
    case TriggerEvent::OUT_EVENT:   m_write_callback = nullptr; break;
    case TriggerEvent::ERROR_EVENT: m_error_callback = nullptr; break;
    }
}

void FdEvent::clearCallbacks() noexcept {
    m_read_callback = nullptr;
    m_write_callback = nullptr;
    m_error_callback = nullptr;
}

bool FdEvent::isListening(TriggerEvent event_type) const noexcept {
    switch (event_type) {
    case TriggerEvent::IN_EVENT:    return static_cast<bool>(m_read_callback);
    case TriggerEvent::OUT_EVENT:   return static_cast<bool>(m_write_callback);
    case TriggerEvent::ERROR_EVENT: return static_cast<bool>(m_error_callback);
    default:                        return false;
    }
}

std::uint32_t FdEvent::listenMask() const noexcept {
    std::uint32_t mask = 0;
    if (m_read_callback)  mask |= toMask(TriggerEvent::IN_EVENT);
    if (m_write_callback) mask |= toMask(TriggerEvent::OUT_EVENT);
    if (m_error_callback) mask |= toMask(TriggerEvent::ERROR_EVENT);
    return mask;
}

} // namespace rocket
