#include "rocket/net/timing_wheel.h"

#include <algorithm>
#include <chrono>
#include <mutex>
#include <vector>

namespace rocket {

void TimingWheel::addEvent(const TimerEvent::s_ptr& event) {
    if (!event || event->isCancelled()) return;
    std::lock_guard<std::mutex> lk(m_mutex);
    if (m_last_tick_ms == 0) {
        m_last_tick_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }
    schedule(event);
}

void TimingWheel::cancelEvent(const TimerEvent::s_ptr& event) {
    if (!event) return;
    event->cancel(); // lazy
}

std::optional<std::int64_t> TimingWheel::msUntilNextExpire() const {
    std::lock_guard<std::mutex> lk(m_mutex);

    auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    std::optional<std::int64_t> next;

    // Check L1 (scan from cursor forward).  The current slot is processed on
    // the next full tick, not immediately.
    const std::int64_t elapsed =
        m_last_tick_ms == 0 ? 0 : std::max<std::int64_t>(0, now - m_last_tick_ms);
    const std::int64_t until_next_tick =
        std::max<std::int64_t>(1, kTickMs - std::min(elapsed, kTickMs));
    for (int i = 0; i < kSlotsPerLevel; ++i) {
        int idx = (m_l1_cursor + i) % kSlotsPerLevel;
        if (!m_l1[idx].empty()) {
            next = until_next_tick + i * kTickMs;
            break;
        }
    }

    // Long timers live in the ordered overflow map.  Compare them with L1
    // instead of blindly preferring a newly-added L1 timer.
    if (!m_overflow.empty()) {
        auto earliest = m_overflow.begin()->first;
        const auto overflow_wait = std::max<std::int64_t>(0, earliest - now);
        next = next.has_value() ? std::min(*next, overflow_wait) : overflow_wait;
    }

    return next;
}

void TimingWheel::fireExpired(std::int64_t now_ms) {
    std::vector<TimerEvent::s_ptr> expired;
    {
        std::lock_guard<std::mutex> lk(m_mutex);

        if (m_last_tick_ms == 0) {
            // First call — initialise the clock.
            m_last_tick_ms = now_ms;
            return;
        }

        std::int64_t elapsed = now_ms - m_last_tick_ms;
        if (elapsed < 0) elapsed = 0; // clock skew guard

        std::int64_t ticks = elapsed / kTickMs;
        if (ticks > static_cast<std::int64_t>(kSlotsPerLevel)) {
            // Large gap (e.g. system suspend). Process directly from overflow
            // and reset the wheel rather than spinning millions of ticks.
            ticks = kSlotsPerLevel;
            m_last_tick_ms = now_ms;
        } else {
            // Preserve the sub-tick remainder.  Advancing this timestamp to
            // now when ticks == 0 would discard every short poll interval and
            // could prevent the wheel cursor from ever moving.
            m_last_tick_ms += ticks * kTickMs;
        }

        for (std::int64_t t = 0; t < ticks; ++t) {
            collectExpired(m_l1[m_l1_cursor], expired);
            tickL1();
        }

        // Fire overflow entries that have expired.
        while (!m_overflow.empty() && m_overflow.begin()->first <= now_ms) {
            auto it = m_overflow.begin();
            if (!it->second->isCancelled()) expired.push_back(it->second);
            m_overflow.erase(it);
        }
    }

    // Fire outside the lock.
    for (auto& ev : expired) {
        if (ev->isCancelled()) continue;
        if (ev->getCallback()) ev->getCallback()();
        if (ev->isRepeated() && !ev->isCancelled()) {
            ev->resetArriveTime();
            addEvent(ev);
        }
    }
}

std::size_t TimingWheel::pendingCount() const {
    std::lock_guard<std::mutex> lk(m_mutex);
    std::size_t count = m_overflow.size();
    for (auto& slot : m_l1) count += slot.size();
    return count;
}

// ─── Private helpers ───────────────────────────────────────────────────

void TimingWheel::schedule(const TimerEvent::s_ptr& event) {
    auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();

    std::int64_t arrival = event->getArriveTime();
    std::int64_t delay = arrival - now;

    if (delay <= 0) {
        // Immediate — put in current L1 slot (fires next tick).
        delay = kTickMs;
        arrival = now + delay;
    }

    if (delay <= kL1Range) {
        const auto ticks_ahead = (delay - 1) / kTickMs;
        int idx = (m_l1_cursor + static_cast<int>(ticks_ahead)) % kSlotsPerLevel;
        m_l1[idx].push_back(event);
    } else {
        // An ordered map is used for long timers.  This avoids delaying a
        // timer by a whole L2 revolution when it is inserted part-way through
        // the current L1 cycle.
        m_overflow.insert({arrival, event});
    }
}

void TimingWheel::tickL1() {
    m_l1[m_l1_cursor].clear();
    m_l1_cursor = (m_l1_cursor + 1) % kSlotsPerLevel;
}

void TimingWheel::collectExpired(Slot& slot, std::vector<TimerEvent::s_ptr>& out) {
    for (auto& ev : slot)
        if (!ev->isCancelled()) out.push_back(ev);
    // Remove cancelled events that would otherwise linger in the slot,
    // causing msUntilNextExpire to return small timeouts → 100% CPU busy-loop.
    std::erase_if(slot, [](const TimerEvent::s_ptr& ev) { return ev->isCancelled(); });
}

} // namespace rocket
