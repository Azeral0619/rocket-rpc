#include "rocket/net/poller/kqueue_poller.h"

#if defined(__APPLE__)

#include "rocket/common/log.h"
#include <cerrno>
#include <cstring>
#include <sys/event.h>
#include <sys/time.h>
#include <unistd.h>

namespace rocket {

namespace {
constexpr int kMaxEvents = 1024;
}

std::unique_ptr<KqueuePoller> KqueuePoller::create() {
    const int kqueue_fd = ::kqueue();
    if (kqueue_fd < 0) {
        ROCKET_LOG_ERROR("kqueue() failed, errno={}, error={}", errno, strerror(errno));
        return nullptr;
    }
    return std::unique_ptr<KqueuePoller>(new KqueuePoller(kqueue_fd));
}

KqueuePoller::KqueuePoller(int kqueue_fd) : m_kqueue_fd(kqueue_fd) {}

KqueuePoller::~KqueuePoller() {
    if (m_kqueue_fd >= 0) {
        ::close(m_kqueue_fd);
    }
}

void KqueuePoller::applyDelta(FdEvent* event, std::uint32_t old_mask, std::uint32_t new_mask) {
    const int fd = event->getFd();

    struct FilterChange {
        int16_t filter;
        std::uint32_t bit;
    };
    constexpr FilterChange kChanges[] = {
        {EVFILT_READ, toMask(FdEvent::TriggerEvent::IN_EVENT)},
        {EVFILT_WRITE, toMask(FdEvent::TriggerEvent::OUT_EVENT)},
    };

    for (const auto& [filter, bit] : kChanges) {
        const bool was_on = (old_mask & bit) != 0;
        const bool want_on = (new_mask & bit) != 0;
        if (was_on == want_on) {
            continue;
        }

        struct kevent ev {};
        // udata carries the FdEvent* so poll() can map events back.
        EV_SET(&ev, static_cast<uintptr_t>(fd), filter, want_on ? (EV_ADD | EV_ENABLE) : EV_DELETE, 0, 0, event);
        if (::kevent(m_kqueue_fd, &ev, 1, nullptr, 0, nullptr) < 0) {
            ROCKET_LOG_ERROR("kevent {} fd={} filter={} failed, errno={}, error={}",
                             want_on ? "EV_ADD" : "EV_DELETE", fd, filter, errno, strerror(errno));
        }
    }
}

int KqueuePoller::poll(int timeout_ms, std::vector<ActiveEvent>& out) {
    out.clear();

    static thread_local std::vector<struct kevent> events(kMaxEvents);

    struct timespec ts {};
    struct timespec* ts_ptr = nullptr;
    if (timeout_ms >= 0) {
        ts.tv_sec = timeout_ms / 1000;
        ts.tv_nsec = static_cast<long>(timeout_ms % 1000) * 1000000L;
        ts_ptr = &ts;
    }

    const int nfds = ::kevent(m_kqueue_fd, nullptr, 0, events.data(), kMaxEvents, ts_ptr);
    if (nfds < 0) {
        if (errno != EINTR) {
            ROCKET_LOG_ERROR("kevent wait failed, errno={}, error={}", errno, strerror(errno));
            return -1;  // fatal
        }
        return 0;
    }

    out.reserve(static_cast<std::size_t>(nfds));
    for (int i = 0; i < nfds; ++i) {
        const struct kevent& ev = events[static_cast<std::size_t>(i)];
        auto* fd_event = static_cast<FdEvent*>(ev.udata);

        std::uint32_t mask = 0;
        if (ev.filter == EVFILT_READ) {
            mask |= toMask(FdEvent::TriggerEvent::IN_EVENT);
        } else if (ev.filter == EVFILT_WRITE) {
            mask |= toMask(FdEvent::TriggerEvent::OUT_EVENT);
        }
        if ((ev.flags & (EV_ERROR | EV_EOF)) != 0) {
            mask |= toMask(FdEvent::TriggerEvent::ERROR_EVENT);
        }
        out.push_back({fd_event, mask});
    }
    return nfds;
}

void KqueuePoller::updateFdEvent(FdEvent* event) {
    if (event == nullptr || event->getFd() < 0) {
        return;
    }

    const int fd = event->getFd();
    const std::uint32_t new_mask = event->listenMask();
    const std::uint32_t old_mask = m_registered.contains(fd) ? m_registered[fd] : 0;

    applyDelta(event, old_mask, new_mask);
    m_registered[fd] = new_mask;
}

void KqueuePoller::removeFdEvent(FdEvent* event) {
    if (event == nullptr || event->getFd() < 0) {
        return;
    }

    const int fd = event->getFd();
    if (!m_registered.contains(fd)) {
        return;
    }

    applyDelta(event, m_registered[fd], 0);
    m_registered.erase(fd);
}

} // namespace rocket

#endif // __APPLE__
