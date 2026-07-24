#include "rocket/net/poller/wakeup_channel.h"

#include "rocket/common/log.h"
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <unistd.h>

#if defined(__linux__)
#include <sys/eventfd.h>
#endif

namespace rocket {

namespace {

#if defined(__linux__)

class EventFdWakeupChannel final : public WakeupChannel {
  public:
    EventFdWakeupChannel() : m_fd(::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)) {}

    ~EventFdWakeupChannel() override {
        if (m_fd >= 0) {
            ::close(m_fd);
        }
    }

    [[nodiscard]] bool isValid() const noexcept override { return m_fd >= 0; }

    [[nodiscard]] int readFd() const noexcept override { return m_fd; }

    void wakeup() override {
        const std::uint64_t val = 1;
        [[maybe_unused]] const ssize_t n = ::write(m_fd, &val, sizeof(val));
    }

    void drain() override {
        std::uint64_t val = 0;
        // eventfd counters aggregate; one read drains everything.
        while (::read(m_fd, &val, sizeof(val)) > 0) {
        }
    }

  private:
    int m_fd{-1};
};

#elif defined(__APPLE__)

class PipeWakeupChannel final : public WakeupChannel {
  public:
    PipeWakeupChannel() {
        int fds[2] = {-1, -1};
        if (::pipe(fds) < 0) {
            return;
        }
        m_read_fd = fds[0];
        m_write_fd = fds[1];
        for (const int fd : {m_read_fd, m_write_fd}) {
            const int flags = ::fcntl(fd, F_GETFL, 0);
            if (flags >= 0) {
                ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
            }
            const int fd_flags = ::fcntl(fd, F_GETFD, 0);
            if (fd_flags >= 0) {
                ::fcntl(fd, F_SETFD, fd_flags | FD_CLOEXEC);
            }
        }
    }

    ~PipeWakeupChannel() override {
        if (m_read_fd >= 0) {
            ::close(m_read_fd);
        }
        if (m_write_fd >= 0) {
            ::close(m_write_fd);
        }
    }

    [[nodiscard]] bool isValid() const noexcept override { return m_read_fd >= 0 && m_write_fd >= 0; }

    [[nodiscard]] int readFd() const noexcept override { return m_read_fd; }

    void wakeup() override {
        const char byte = 'W';
        [[maybe_unused]] const ssize_t n = ::write(m_write_fd, &byte, 1);
    }

    void drain() override {
        char buf[64];
        while (::read(m_read_fd, buf, sizeof(buf)) > 0) {
        }
    }

  private:
    int m_read_fd{-1};
    int m_write_fd{-1};
};

#else
#error "rocket::WakeupChannel: unsupported platform (only Linux and macOS are supported)"
#endif

} // anonymous namespace

std::unique_ptr<WakeupChannel> WakeupChannel::create() {
#if defined(__linux__)
    auto channel = std::make_unique<EventFdWakeupChannel>();
#elif defined(__APPLE__)
    auto channel = std::make_unique<PipeWakeupChannel>();
#endif
    if (!channel->isValid()) {
        ROCKET_LOG_ERROR("WakeupChannel create failed, errno={}, error={}", errno, strerror(errno));
        return nullptr;
    }
    return channel;
}

} // namespace rocket
