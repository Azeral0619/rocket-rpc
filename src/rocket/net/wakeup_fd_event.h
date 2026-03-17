#pragma once

#include "rocket/net/fd_event.h"

namespace rocket {

class WakeUpFdEvent : public FdEvent {
  public:
    WakeUpFdEvent();

    explicit WakeUpFdEvent(int fd);

    ~WakeUpFdEvent() = default;

    WakeUpFdEvent(const WakeUpFdEvent&) = delete;
    WakeUpFdEvent& operator=(const WakeUpFdEvent&) = delete;
    WakeUpFdEvent(WakeUpFdEvent&&) = delete;
    WakeUpFdEvent& operator=(WakeUpFdEvent&&) = delete;

    void wakeup();

  private:
    void listenWakeup();
};

} // namespace rocket