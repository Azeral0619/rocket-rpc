#pragma once

// macOS kqueue backend for Poller. Compiles to an empty TU on other platforms.

#if defined(__APPLE__)

#include "rocket/net/poller/poller.h"
#include <unordered_map>

namespace rocket {

class KqueuePoller final : public Poller {
  public:
    KqueuePoller() = delete;

    // Returns nullptr if kqueue creation fails.
    [[nodiscard]] static std::unique_ptr<KqueuePoller> create();

    ~KqueuePoller() override;

    int poll(int timeout_ms, std::vector<ActiveEvent>& out) override;

    void updateFdEvent(FdEvent* event) override;

    void removeFdEvent(FdEvent* event) override;

  private:
    explicit KqueuePoller(int kqueue_fd);

    // Apply the delta between the currently registered mask and the desired one.
    void applyDelta(FdEvent* event, std::uint32_t old_mask, std::uint32_t new_mask);

    int m_kqueue_fd{-1};
    // fd -> currently registered listen mask, to compute EV_ADD/EV_DELETE deltas.
    std::unordered_map<int, std::uint32_t> m_registered;
};

} // namespace rocket

#endif // __APPLE__
