#include "rocket/net/io_thread.h"
#include "rocket/common/log.h"
#include "rocket/net/event_loop.h"
#include <memory>
#include <mutex>
#include <thread>

namespace rocket {

IOThread::IOThread() = default;

IOThread::~IOThread() {
    if (m_event_loop != nullptr) {
        m_event_loop->stop();
    }
    if (m_thread && m_thread->joinable()) {
        m_thread->join();
    }
}

void IOThread::threadFunc() {
    EventLoop loop;

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_event_loop = &loop;
        m_thread_id = std::this_thread::get_id();
        m_initialized = true;
        ROCKET_LOG_DEBUG("IOThread [{}] create success", m_thread_id);
    }
    m_init_cv.notify_one();

    {
        std::unique_lock<std::mutex> lock(m_mutex);
        ROCKET_LOG_DEBUG("IOThread [{}] created, wait start cv", m_thread_id);
        m_start_cv.wait(lock, [this] { return m_started; });
    }
    ROCKET_LOG_DEBUG("IOThread [{}] start eventloop", m_thread_id);
    loop.loop();
    ROCKET_LOG_DEBUG("IOThread [{}] end eventloop", m_thread_id);
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_event_loop = nullptr;
    }
}

EventLoop* IOThread::getEventLoop() const noexcept { return m_event_loop; }

void IOThread::start() {
    m_thread = std::make_unique<std::thread>(&IOThread::threadFunc, this);

    {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_init_cv.wait(lock, [this] { return m_initialized; });
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_started = true;
    }
    ROCKET_LOG_DEBUG("Invoke IOThread [{}]", m_thread_id);
    m_start_cv.notify_one();
}

void IOThread::join() {
    if (m_event_loop != nullptr) {
        m_event_loop->stop();
    }
    if (m_thread && m_thread->joinable()) {
        m_thread->join();
    }
}

bool IOThread::isRunning() const noexcept { return m_thread && m_thread->joinable(); }

std::thread::id IOThread::getThreadId() const noexcept { return m_thread_id; }

} // namespace rocket
