#include "rocket/net/timer_event.h"
#include <chrono>
#include <cstdint>
#include <functional>
#include <new>
#include <utility>

namespace rocket {

// ── Thread-local object pool ────────────────────────────────────────────
// Recycles TimerEvent memory across RPC calls.  The custom deleter
// calls ~TimerEvent() (releasing captured shared_ptrs in the callback),
// then returns the raw memory to the pool.  create() extracts from the
// pool and placement-new constructs a fresh object.

namespace {

struct TimerEventSlot {
    TimerEventSlot* next;
};

constexpr std::size_t kTimerPoolMax = 64;

thread_local TimerEventSlot* t_pool_head = nullptr;
thread_local std::size_t t_pool_size = 0;

void timerDeleter(TimerEvent* ev) {
    ev->~TimerEvent();
    if (t_pool_size < kTimerPoolMax) {
        auto* slot = reinterpret_cast<TimerEventSlot*>(ev);
        slot->next = t_pool_head;
        t_pool_head = slot;
        ++t_pool_size;
    } else {
        ::operator delete(ev);
    }
}

} // namespace

TimerEvent::TimerEvent(std::int64_t interval, bool is_repeated, std::function<void()> cb)
    : m_interval(interval), m_is_repeated(is_repeated), m_task(std::move(cb)) {
    resetArriveTime();
}

TimerEvent::s_ptr TimerEvent::create(std::int64_t interval, bool is_repeated, std::function<void()> cb) {
    TimerEvent* ev;
    if (t_pool_head) {
        void* p = t_pool_head;
        t_pool_head = t_pool_head->next;
        --t_pool_size;
        ev = ::new (p) TimerEvent(interval, is_repeated, std::move(cb));
    } else {
        ev = new TimerEvent(interval, is_repeated, std::move(cb));
    }
    return s_ptr(ev, timerDeleter);
}

void TimerEvent::resetArriveTime() {
    const auto now = std::chrono::steady_clock::now();
    const auto ms = std::chrono::time_point_cast<std::chrono::milliseconds>(now);
    m_arrive_time = ms.time_since_epoch().count() + m_interval;
}

} // namespace rocket
