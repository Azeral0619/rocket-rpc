#pragma once

#include "rocket/net/timer_event.h"
#include <array>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <vector>

namespace rocket {

// Timing wheel with an ordered long-timer overflow.
//
// Level 1: 256 slots × 10 ms tick = 2.56 s range (sub-ms precision not needed;
//          the wheel rounds to tick boundaries.)
//
// Events beyond the wheel range use an overflow multimap.
// Add / cancel are O(1) (amortised).  Fire is O(N) in the number of expired
// events per tick.
//
// Thread safety: all public methods are guarded by an internal mutex.
class TimingWheel {
  public:
    static constexpr std::int64_t kTickMs = 10;
    static constexpr int kSlotsPerLevel = 256;
    static constexpr std::int64_t kL1Range = kSlotsPerLevel * kTickMs;       // 2560 ms

    TimingWheel() = default;

    // Insert or re-insert an event.  Called for both new and repeated events.
    void addEvent(const TimerEvent::s_ptr& event);

    // Lazy cancel — just marks the event; fireExpired skips it.
    void cancelEvent(const TimerEvent::s_ptr& event);

    // Milliseconds until the next non-empty slot, or nullopt if idle.
    [[nodiscard]] std::optional<std::int64_t> msUntilNextExpire() const;

    // Advance the wheel to `now_ms` (steady_clock epoch ms) and fire all
    // expired, non-cancelled events.  Repeated events are re-inserted.
    void fireExpired(std::int64_t now_ms);

    [[nodiscard]] std::size_t pendingCount() const;

  private:
    using Slot = std::vector<TimerEvent::s_ptr>;

    // Advance L1 by one tick.
    void tickL1();

    // Put an event into the right level (or overflow).
    void schedule(const TimerEvent::s_ptr& event);

    void collectExpired(Slot& slot, std::vector<TimerEvent::s_ptr>& out);

    std::array<Slot, kSlotsPerLevel> m_l1;
    std::multimap<std::int64_t, TimerEvent::s_ptr> m_overflow; // key = arrival ms

    int m_l1_cursor{0};
    std::int64_t m_last_tick_ms{0}; // last time fireExpired was called

    mutable std::mutex m_mutex;
};

} // namespace rocket
