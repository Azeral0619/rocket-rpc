#include "rocket/net/poller/epoll_poller.h"

#if defined(__linux__)

#include "rocket/common/log.h"
#include <cerrno>
#include <cstring>
#include <sys/epoll.h>
#include <unistd.h>

namespace rocket {

namespace {
constexpr int kMaxEvents = 1024;
}

std::unique_ptr<EPollPoller> EPollPoller::create() {
    const int epoll_fd = ::epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd < 0) {
        ROCKET_LOG_ERROR("epoll_create1 failed, errno={}, error={}", errno, strerror(errno));
        return nullptr;
    }
    return std::unique_ptr<EPollPoller>(new EPollPoller(epoll_fd));
}

EPollPoller::EPollPoller(int epoll_fd) : m_epoll_fd(epoll_fd) {}

EPollPoller::~EPollPoller() {
    if (m_epoll_fd >= 0) {
        ::close(m_epoll_fd);
    }
}

std::uint32_t EPollPoller::toEpollEvents(std::uint32_t listen_mask) noexcept {
    std::uint32_t events = 0;
    if ((listen_mask & toMask(FdEvent::TriggerEvent::IN_EVENT)) != 0) {
        events |= EPOLLIN;
    }
    if ((listen_mask & toMask(FdEvent::TriggerEvent::OUT_EVENT)) != 0) {
        events |= EPOLLOUT;
    }
    // EPOLLERR/EPOLLHUP are always reported by epoll, no need to request them.
    return events;
}

int EPollPoller::poll(int timeout_ms, std::vector<ActiveEvent>& out) {
    out.clear();

    static thread_local std::vector<epoll_event> events(kMaxEvents);

    const int nfds = ::epoll_wait(m_epoll_fd, events.data(), kMaxEvents, timeout_ms);
    if (nfds < 0) {
        if (errno != EINTR) {
            ROCKET_LOG_ERROR("epoll_wait failed, errno={}, error={}", errno, strerror(errno));
            return -1;  // fatal
        }
        return 0;
    }

    out.reserve(static_cast<std::size_t>(nfds));
    for (int i = 0; i < nfds; ++i) {
        const epoll_event& ev = events[static_cast<std::size_t>(i)];
        auto* fd_event = static_cast<FdEvent*>(ev.data.ptr);

        std::uint32_t mask = 0;
        if ((ev.events & EPOLLIN) != 0) {
            mask |= toMask(FdEvent::TriggerEvent::IN_EVENT);
        }
        if ((ev.events & EPOLLOUT) != 0) {
            mask |= toMask(FdEvent::TriggerEvent::OUT_EVENT);
        }
        if ((ev.events & (EPOLLERR | EPOLLHUP)) != 0) {
            mask |= toMask(FdEvent::TriggerEvent::ERROR_EVENT);
        }
        out.push_back({fd_event, mask});
    }
    return nfds;
}

void EPollPoller::updateFdEvent(FdEvent* event) {
    if (event == nullptr || event->getFd() < 0) {
        return;
    }

    const int fd = event->getFd();

    epoll_event ev{};
    ev.events = toEpollEvents(event->listenMask());
    ev.data.ptr = event;

    if (!m_registered.contains(fd)) {
        if (::epoll_ctl(m_epoll_fd, EPOLL_CTL_ADD, fd, &ev) < 0) {
            ROCKET_LOG_ERROR("epoll_ctl ADD fd={} failed, errno={}, error={}", fd, errno, strerror(errno));
            return;
        }
    } else {
        if (::epoll_ctl(m_epoll_fd, EPOLL_CTL_MOD, fd, &ev) < 0) {
            ROCKET_LOG_ERROR("epoll_ctl MOD fd={} failed, errno={}, error={}", fd, errno, strerror(errno));
            return;
        }
    }
    m_registered[fd] = event->listenMask();
}

void EPollPoller::removeFdEvent(FdEvent* event) {
    if (event == nullptr || event->getFd() < 0) {
        return;
    }

    const int fd = event->getFd();
    if (!m_registered.contains(fd)) {
        return;
    }

    if (::epoll_ctl(m_epoll_fd, EPOLL_CTL_DEL, fd, nullptr) < 0) {
        ROCKET_LOG_ERROR("epoll_ctl DEL fd={} failed, errno={}, error={}", fd, errno, strerror(errno));
    }
    m_registered.erase(fd);
}

} // namespace rocket

#endif // __linux__
