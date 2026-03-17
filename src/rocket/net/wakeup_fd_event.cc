#include "rocket/net/wakeup_fd_event.h"
#include "rocket/net/fd_event.h"
#include <cstdint>
#include <sys/eventfd.h>
#include <sys/types.h>
#include <unistd.h>

namespace rocket {

WakeUpFdEvent::WakeUpFdEvent() {
    m_fd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (m_fd < 0) {
        return;
    }

    listenWakeup();
}

WakeUpFdEvent::WakeUpFdEvent(int fd) : FdEvent(fd) { listenWakeup(); }

void WakeUpFdEvent::wakeup() {
    const std::uint64_t val = 1;
    [[maybe_unused]] const ssize_t n = ::write(m_fd, &val, sizeof(val));
}

void WakeUpFdEvent::listenWakeup() {
    listen(TriggerEvent::IN_EVENT, [this]() {
        std::uint64_t val = 0;
        [[maybe_unused]] const ssize_t n = ::read(m_fd, &val, sizeof(val));
    });
}

} // namespace rocket
