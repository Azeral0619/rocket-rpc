#include "rocket/net/io_thread_group.h"
#include "rocket/common/log.h"
#include "rocket/net/io_thread.h"
#include <cstddef>
#include <memory>

namespace rocket {

IOThreadGroup::IOThreadGroup(std::size_t size) : m_size(size) {
    m_io_threads.reserve(size);
    for (std::size_t i = 0; i < size; ++i) {
        m_io_threads.push_back(std::make_unique<IOThread>());
    }
    ROCKET_LOG_DEBUG("IOThreadGroup created with {} threads", size);
}

IOThreadGroup::~IOThreadGroup() { ROCKET_LOG_DEBUG("IOThreadGroup destroyed"); }

void IOThreadGroup::start() {
    for (const auto& thread : m_io_threads) {
        thread->start();
    }
    ROCKET_LOG_INFO("IOThreadGroup started {} threads", m_size);
}

void IOThreadGroup::join() {
    for (const auto& thread : m_io_threads) {
        thread->join();
    }
    ROCKET_LOG_INFO("IOThreadGroup joined all threads");
}

IOThread* IOThreadGroup::getIOThread() noexcept {
    if (m_io_threads.empty()) {
        return nullptr;
    }
    auto* thread = m_io_threads[m_index].get();
    m_index = (m_index + 1) % m_size;
    return thread;
}

std::size_t IOThreadGroup::getIOThreadSize() const noexcept { return m_size; }

} // namespace rocket