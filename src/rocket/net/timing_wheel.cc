#include "rocket/net/timing_wheel.h"

#include <algorithm>
#include <chrono>
#include <mutex>
#include <vector>

namespace rocket {

void TimingWheel::addEvent(const TimerEvent::s_ptr& event) {
    if (!event || event->isCancelled()) return;
    std::lock_guard<std::mutex> lk(m_mutex);
    schedule(event);
}

void TimingWheel::cancelEvent(const TimerEvent::s_ptr& event) {
    if (!event) return;
    event->cancel(); // lazy
}

std::optional<std::int64_t> TimingWheel::msUntilNextExpire() const {
    std::lock_guard<std::mutex> lk(m_mutex);

    // Check L1 (scan from cursor forward)
    for (int i = 0; i < kSlotsPerLevel; ++i) {
        int idx = (m_l1_cursor + i) % kSlotsPerLevel;
        if (!m_l1[idx].empty()) return i * kTickMs;
    }

    // Check L2
    if (!m_overflow.empty()) {
        // Overflow sorted by arrival time — return time until first entry.
        auto earliest = m_overflow.begin()->first;
        auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        return std::max<std::int64_t>(0, earliest - now);
    }

    // L2 is scanned from cursor
    for (int i = 0; i < kSlotsPerLevel; ++i) {
        int idx = (m_l2_cursor + i) % kSlotsPerLevel;
        if (!m_l2[idx].empty()) return kL1Range - (m_l1_cursor * kTickMs) + i * kL1Range;
    }

    return std::nullopt; // idle
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
        m_last_tick_ms = now_ms;

        std::int64_t ticks = elapsed / kTickMs;
        if (ticks > static_cast<std::int64_t>(kSlotsPerLevel * 2)) {
            // Large gap (e.g. system suspend). Process directly from overflow
            // and reset the wheel rather than spinning millions of ticks.
            ticks = kSlotsPerLevel;
        }

        for (std::int64_t t = 0; t < ticks; ++t) {
            collectExpired(m_l1[m_l1_cursor], expired);
            if (tickL1()) cascadeL2();
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
    for (auto& slot : m_l2) count += slot.size();
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
        int idx = (m_l1_cursor + (delay / kTickMs)) % kSlotsPerLevel;
        m_l1[idx].push_back(event);
    } else if (delay <= kL2Range) {
        int idx = (m_l2_cursor + (delay / kL1Range)) % kSlotsPerLevel;
        m_l2[idx].push_back(event);
    } else {
        m_overflow.insert({arrival, event});
    }
}

bool TimingWheel::tickL1() {
    m_l1[m_l1_cursor].clear();
    m_l1_cursor = (m_l1_cursor + 1) % kSlotsPerLevel;
    return m_l1_cursor == 0; // wrapped
}

void TimingWheel::cascadeL2() {
    auto& slot = m_l2[m_l2_cursor];
    for (auto& ev : slot)
        schedule(ev); // re-insert into L1 (or overflow)
    slot.clear();
    m_l2_cursor = (m_l2_cursor + 1) % kSlotsPerLevel;
}

void TimingWheel::collectExpired(Slot& slot, std::vector<TimerEvent::s_ptr>& out) {
    for (auto& ev : slot)
        if (!ev->isCancelled()) out.push_back(ev);
    // Remove cancelled events that would otherwise linger in the slot,
    // causing msUntilNextExpire to return small timeouts → 100% CPU busy-loop.
    std::erase_if(slot, [](const TimerEvent::s_ptr& ev) { return ev->isCancelled(); });
}

} // namespace rocket
