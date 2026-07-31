#include "rocket/net/io_thread_group.h"
#include "rocket/common/log.h"
#include "rocket/net/io_thread.h"
#include <atomic>
#include <cstddef>
#include <memory>

namespace rocket {

namespace {

// Keep independently-created IO groups off the same cores.  A process may
// host both a server group and a client-pool group; restarting the index at
// zero for every group pinned all of them to the same CPUs.
std::atomic<std::size_t> g_next_affinity_index{0};

} // namespace

IOThreadGroup::IOThreadGroup(std::size_t size) : m_size(size) {
    m_io_threads.reserve(size);
    for (std::size_t i = 0; i < size; ++i) {
        m_io_threads.push_back(std::make_unique<IOThread>());
    }
    ROCKET_LOG_DEBUG("IOThreadGroup created with {} threads", size);
}

IOThreadGroup::~IOThreadGroup() { ROCKET_LOG_DEBUG("IOThreadGroup destroyed"); }

void IOThreadGroup::start() {
    const auto affinity_base =
        g_next_affinity_index.fetch_add(m_io_threads.size(),
                                        std::memory_order_relaxed);
    for (std::size_t i = 0; i < m_io_threads.size(); ++i) {
        m_io_threads[i]->start(affinity_base + i);
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

IOThread* IOThreadGroup::getIOThreadAt(std::size_t i) noexcept {
    if (i >= m_io_threads.size()) return nullptr;
    return m_io_threads[i].get();
}

std::size_t IOThreadGroup::getIOThreadSize() const noexcept { return m_size; }

} // namespace rocket
