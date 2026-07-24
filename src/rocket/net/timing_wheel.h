#pragma once

#include "rocket/net/timer_event.h"
#include <array>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <vector>

namespace rocket {

// Two-level hierarchical timing wheel.
//
// Level 1: 256 slots × 10 ms tick = 2.56 s range (sub-ms precision not needed;
//          the wheel rounds to tick boundaries.)
// Level 2: 256 slots × 2.56 s     = ~655 s range
//
// Events beyond L2 range fall into an overflow multimap (rare in practice).
// Add / cancel are O(1) (amortised).  Fire is O(N) in the number of expired
// events per tick.
//
// Thread safety: all public methods are guarded by an internal mutex.
class TimingWheel {
  public:
    static constexpr std::int64_t kTickMs = 10;
    static constexpr int kSlotsPerLevel = 256;
    static constexpr std::int64_t kL1Range = kSlotsPerLevel * kTickMs;       // 2560 ms
    static constexpr std::int64_t kL2Range = kSlotsPerLevel * kL1Range;      // 655 360 ms

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

    // Advance L1 by one tick.  Returns true if L1 wrapped around.
    bool tickL1();

    // Cascade one slot from L2 down into L1.
    void cascadeL2();

    // Put an event into the right level (or overflow).
    void schedule(const TimerEvent::s_ptr& event);

    void collectExpired(Slot& slot, std::vector<TimerEvent::s_ptr>& out);

    std::array<Slot, kSlotsPerLevel> m_l1;
    std::array<Slot, kSlotsPerLevel> m_l2;
    std::multimap<std::int64_t, TimerEvent::s_ptr> m_overflow; // key = arrival ms

    int m_l1_cursor{0};
    int m_l2_cursor{0};
    std::int64_t m_last_tick_ms{0}; // last time fireExpired was called

    mutable std::mutex m_mutex;
};

} // namespace rocket
