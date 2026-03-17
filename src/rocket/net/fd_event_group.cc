#include "rocket/net/fd_event_group.h"
#include "rocket/net/fd_event.h"
#include <algorithm>
#include <cstddef>
#include <memory>
#include <mutex>

namespace rocket {

FdEventGroup::FdEventGroup() : FdEventGroup(kDefaultSize) {}

FdEventGroup::FdEventGroup(std::size_t size) : m_size(size) {
    m_fd_group.resize(size);
    for (std::size_t i = 0; i < size; ++i) {
        m_fd_group[i] = std::make_unique<FdEvent>(i);
    }
}

FdEvent* FdEventGroup::getFdEvent(int fd) {
    std::lock_guard<std::mutex> lock(m_mutex);

    if (fd < 0) {
        return nullptr;
    }

    const auto fd_size = static_cast<std::size_t>(fd);

    if (fd_size >= m_size) {
        const std::size_t new_size = std::max(fd_size + 1, m_size * 2);
        m_fd_group.resize(new_size);

        for (std::size_t i = m_size; i < new_size; ++i) {
            m_fd_group[i] = std::make_unique<FdEvent>();
        }

        m_size = new_size;
    }

    return m_fd_group[fd_size].get();
}

} // namespace rocket
