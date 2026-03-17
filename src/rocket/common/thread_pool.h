#pragma once

#include <concepts>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <future>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace rocket {

class ThreadPool {
  public:
    static constexpr std::size_t kDefaultThreadCount = 4;

    explicit ThreadPool(std::size_t threads = kDefaultThreadCount);

    ~ThreadPool();

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;
    ThreadPool(ThreadPool&&) = delete;
    ThreadPool& operator=(ThreadPool&&) = delete;

    template <typename F, typename... Args>
        requires std::invocable<F, Args...>
    [[nodiscard]] auto submit(F&& f, Args&&... args) -> std::future<std::invoke_result_t<F, Args...>>;

    void execute(std::function<void()> task);

    [[nodiscard]] std::size_t threadCount() const noexcept { return m_workers.size(); }
    [[nodiscard]] std::size_t pendingCount() const;

  private:
    std::vector<std::jthread> m_workers;
    std::queue<std::function<void()>> m_tasks;

    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    bool m_stop{false};
};

// ─── Implementation ──────────────────────────────────────────────

inline ThreadPool::ThreadPool(std::size_t threads) {
    m_workers.reserve(threads);
    for (std::size_t i = 0; i < threads; ++i) {
        m_workers.emplace_back([this] {
            for (;;) {
                std::function<void()> task;
                {
                    std::unique_lock lock(m_mutex);
                    m_cv.wait(lock, [this] { return m_stop || !m_tasks.empty(); });
                    if (m_stop && m_tasks.empty()) {
                        return;
                    }
                    task = std::move(m_tasks.front());
                    m_tasks.pop();
                }
                task();
            }
        });
    }
}

inline ThreadPool::~ThreadPool() {
    {
        std::lock_guard lock(m_mutex);
        m_stop = true;
    }
    m_cv.notify_all();
}

template <typename F, typename... Args>
    requires std::invocable<F, Args...>
auto ThreadPool::submit(F&& f, Args&&... args) -> std::future<std::invoke_result_t<F, Args...>> {
    using return_type = std::invoke_result_t<F, Args...>;

    auto task = std::make_shared<std::packaged_task<return_type()>>(
        [f = std::forward<F>(f), ... args = std::forward<Args>(args)]() mutable -> return_type {
            return std::invoke(std::move(f), std::move(args)...);
        });

    auto future = task->get_future();
    {
        std::lock_guard lock(m_mutex);
        if (m_stop) {
            throw std::runtime_error("submit on stopped ThreadPool");
        }
        m_tasks.emplace([task = std::move(task)] { (*task)(); });
    }
    m_cv.notify_one();
    return future;
}

inline void ThreadPool::execute(std::function<void()> task) {
    {
        std::lock_guard lock(m_mutex);
        if (m_stop) {
            return;
        }
        m_tasks.emplace(std::move(task));
    }
    m_cv.notify_one();
}

inline std::size_t ThreadPool::pendingCount() const {
    std::lock_guard lock(m_mutex);
    return m_tasks.size();
}

} // namespace rocket
