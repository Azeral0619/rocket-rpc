#include "rocket/net/timer.h"

namespace rocket {

void Timer::addTimerEvent(const TimerEvent::s_ptr& event) { m_wheel.addEvent(event); }

void Timer::deleteTimerEvent(const TimerEvent::s_ptr& event) { m_wheel.cancelEvent(event); }

std::optional<std::int64_t> Timer::msUntilNextExpire() const { return m_wheel.msUntilNextExpire(); }

void Timer::fireExpired(std::int64_t now_ms) { m_wheel.fireExpired(now_ms); }

std::size_t Timer::pendingCount() const { return m_wheel.pendingCount(); }

} // namespace rocket
