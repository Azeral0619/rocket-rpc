#pragma once
#include "rocket/net/io_thread.h"
#include <cstddef>
#include <memory>
#include <vector>

namespace rocket {

class IOThreadGroup {
  public:
    explicit IOThreadGroup(std::size_t size);

    ~IOThreadGroup();

    IOThreadGroup(const IOThreadGroup&) = delete;
    IOThreadGroup& operator=(const IOThreadGroup&) = delete;
    IOThreadGroup(IOThreadGroup&&) = delete;
    IOThreadGroup& operator=(IOThreadGroup&&) = delete;

    void start();

    void join();

    [[nodiscard]] IOThread* getIOThread() noexcept;

    [[nodiscard]] std::size_t getIOThreadSize() const noexcept;

  private:
    std::size_t m_size{0};
    std::vector<std::unique_ptr<IOThread>> m_io_threads;
    std::size_t m_index{0};
};

} // namespace rocket