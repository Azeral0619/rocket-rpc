#pragma once

#include "rocket/net/fd_event.h"
#include "rocket/common/singleton.h"
#include <cstddef>
#include <memory>
#include <mutex>
#include <vector>

namespace rocket {

class FdEventGroup : public Singleton<FdEventGroup> {
    friend class Singleton<FdEventGroup>;

  public:
    ~FdEventGroup() = default;

    FdEventGroup(const FdEventGroup&) = delete;
    FdEventGroup& operator=(const FdEventGroup&) = delete;
    FdEventGroup(FdEventGroup&&) = delete;
    FdEventGroup& operator=(FdEventGroup&&) = delete;

    [[nodiscard]] FdEvent* getFdEvent(int fd);

    [[nodiscard]] std::size_t size() const noexcept { return m_size; }

  private:
    static constexpr std::size_t kDefaultSize = 128;

    FdEventGroup();

    explicit FdEventGroup(std::size_t size);

    std::size_t m_size{0};
    std::vector<std::unique_ptr<FdEvent>> m_fd_group;
    mutable std::mutex m_mutex;
};

} // namespace rocket