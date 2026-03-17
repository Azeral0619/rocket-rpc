#pragma once

#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

namespace rocket {

class EventLoop;

class IOThread {
  public:
    IOThread();

    ~IOThread();

    IOThread(const IOThread&) = delete;
    IOThread& operator=(const IOThread&) = delete;
    IOThread(IOThread&&) = delete;
    IOThread& operator=(IOThread&&) = delete;

    [[nodiscard]] EventLoop* getEventLoop() const noexcept;

    void start();

    void join();

    [[nodiscard]] bool isRunning() const noexcept;

    [[nodiscard]] std::thread::id getThreadId() const noexcept;

  private:
    void threadFunc();

    std::unique_ptr<std::thread> m_thread;
    std::thread::id m_thread_id;
    EventLoop* m_event_loop{nullptr};

    std::mutex m_mutex;
    std::condition_variable m_init_cv;
    std::condition_variable m_start_cv;
    bool m_initialized{false};
    bool m_started{false};
};

} // namespace rocket