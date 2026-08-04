#include "rocket/common/log.h"
#include "rocket/common/config.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <csignal>
#include <cstdint>
#include <ctime>
#include <fcntl.h>
#include <filesystem>
#include <fmt/base.h>
#include <google/protobuf/descriptor.h>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>
#include <fmt/format.h>

#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#endif

#if defined(__x86_64__) || defined(__i386__)
#include <emmintrin.h>
#define cpu_pause() _mm_pause();
#elif defined(__aarch64__)
#define cpu_pause() asm volatile("yield" ::: "memory");
#else
#define cpu_pause()
#endif

namespace rocket {
namespace {

constexpr int K1=1,K2=2,K4=4,K8=8,K16=16,K32=32;

LogLevel ParseLogLevel(std::string_view level) {
    if (level == "DEBUG" || level == "debug" || level == "Debug") return LogLevel::Debug;
    if (level == "INFO"  || level == "info"  || level == "Info")  return LogLevel::Info;
    if (level == "WARN"  || level == "warn"  || level == "Warn")  return LogLevel::Warn;
    if (level == "ERROR" || level == "error" || level == "Error") return LogLevel::Error;
    return LogLevel::Debug;
}

Logger::Options BuildOptionsFromConfig() {
    Logger::Options opts;
    auto cfg = Config::getInstance().getConfig();
    if (!cfg) return opts;
    if (!cfg->log_file_name.empty()) {
        std::filesystem::path fn{cfg->log_file_name};
        opts.file_path = (!cfg->log_file_path.empty() && !fn.is_absolute())
            ? std::filesystem::path{cfg->log_file_path} / fn : fn;
    }
    opts.level = ParseLogLevel(cfg->log_level);
    if (cfg->log_sync_interval > 0) opts.flush_interval_ms = static_cast<std::size_t>(cfg->log_sync_interval);
    if (cfg->log_max_file_size > 0) opts.max_file_size = static_cast<std::size_t>(cfg->log_max_file_size);
    if (cfg->log_queue_capacity > 0) opts.per_thread_queue_bytes = static_cast<std::size_t>(cfg->log_queue_capacity);
    return opts;
}

std::size_t NextPow2(std::size_t v) {
    if (v < 2) return 2;
    v--; v|=v>>K1; v|=v>>K2; v|=v>>K4; v|=v>>K8; v|=v>>K16;
    if constexpr (sizeof(std::size_t) >= sizeof(std::uint64_t)) v|=v>>K32;
    return v + 1;
}

double CalibrateNanosecondsPerLogClockTick() {
#if defined(__aarch64__)
    std::uint64_t frequency{0};
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(frequency));
    return frequency == 0 ? 1.0 : 1'000'000'000.0 / static_cast<double>(frequency);
#elif defined(__x86_64__) || defined(__i386__)
    // Calibrate once per process. A 20 ms window is long enough to make
    // scheduler and clock-read noise insignificant for millisecond log output.
    constexpr auto kWindow = std::chrono::milliseconds{20};
    const auto begin_time = std::chrono::steady_clock::now();
    const auto begin_ticks = detail::ReadLogClockTicks();
    std::this_thread::sleep_until(begin_time + kWindow);
    const auto end_ticks = detail::ReadLogClockTicks();
    const auto end_time = std::chrono::steady_clock::now();
    const auto elapsed_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                end_time - begin_time).count();
    return static_cast<double>(elapsed_ns) /
           static_cast<double>(end_ticks - begin_ticks);
#else
    return 1.0;
#endif
}

double NanosecondsPerLogClockTick() {
    static const double value = CalibrateNanosecondsPerLogClockTick();
    return value;
}

void PinCurrentThread(int cpu) noexcept {
#if defined(__linux__)
    if (cpu < 0 || cpu >= CPU_SETSIZE) return;
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(cpu, &cpuset);
    (void)pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);
#else
    (void)cpu;
#endif
}

} // namespace

// ============================================================================
// Variable-length SPSC byte ring
// ============================================================================

Logger::SpscByteQueue::SpscByteQueue(std::size_t capacity_bytes) {
    const auto n = NextPow2(std::max<std::size_t>(capacity_bytes, 4096));
    m_capacity = n;
    m_mask = n - 1;
    m_buffer = std::make_unique<std::byte[]>(n);
}

Logger::LogEntry* Logger::SpscByteQueue::tryClaim(std::size_t record_size) {
    record_size = (record_size + 7U) & ~std::size_t{7U};
    if (record_size < sizeof(LogEntry) ||
        record_size > m_capacity) {
        return nullptr;
    }

    const std::size_t offset = m_write & m_mask;
    const std::size_t bytes_to_end = m_capacity - offset;
    const std::size_t padding = record_size > bytes_to_end ? bytes_to_end : 0;
    const std::size_t required = padding + record_size;

    std::size_t free_bytes = m_capacity - (m_write - m_cached_read);
    if (required > free_bytes) {
        m_cached_read = m_published_read.load(std::memory_order_acquire);
        free_bytes = m_capacity - (m_write - m_cached_read);
    }

    if (padding >= sizeof(Prefix)) {
        if (padding > free_bytes) return nullptr;

        auto* prefix = ::new (static_cast<void*>(m_buffer.get() + offset))
            Prefix;
        prefix->record_size = static_cast<std::uint32_t>(padding);
        prefix->reserved = 0;
        prefix->level = 0;
        prefix->record_tag = kPaddingRecord;

        // A nearly queue-sized record can require more than one ring's worth
        // of logical space when the tail padding is included. Publish just the
        // padding so the consumer can advance to offset zero; this record is
        // dropped, but following records are not permanently wedged there.
        if (required > free_bytes) {
            m_write += padding;
            m_published_write.store(m_write, std::memory_order_release);
            return nullptr;
        }
    } else if (required > free_bytes) {
        return nullptr;
    }

    const std::size_t record_offset = (m_write + padding) & m_mask;
    auto* entry = ::new (static_cast<void*>(m_buffer.get() + record_offset))
        LogEntry;
    m_pending_write = m_write + required;
    return entry;
}

void Logger::SpscByteQueue::publish() {
    m_write = m_pending_write;
    m_published_write.store(m_write, std::memory_order_release);
}

Logger::LogEntry* Logger::SpscByteQueue::front() {
    for (;;) {
        if (m_read == m_cached_write) {
            m_cached_write =
                m_published_write.load(std::memory_order_acquire);
            if (m_read == m_cached_write) return nullptr;
        }

        const std::size_t offset = m_read & m_mask;
        const auto* prefix = reinterpret_cast<const Prefix*>(
            m_buffer.get() + offset);
        if (prefix->record_tag == kPaddingRecord) {
            m_read += prefix->record_size;
            m_published_read.store(m_read, std::memory_order_release);
            continue;
        }

        auto* entry = reinterpret_cast<LogEntry*>(m_buffer.get() + offset);
        if (entry->record_size < sizeof(LogEntry) ||
            entry->record_size > m_capacity - offset ||
            entry->record_size > m_cached_write - m_read) [[unlikely]] {
            return nullptr;
        }
        return entry;
    }
}

void Logger::SpscByteQueue::pop(const LogEntry& entry) {
    m_read += entry.record_size;
    m_published_read.store(m_read, std::memory_order_release);
}

bool Logger::SpscByteQueue::empty() const noexcept {
    return m_read == m_published_write.load(std::memory_order_acquire);
}

// ============================================================================
// Logger
// ============================================================================

Logger::~Logger() { stop(); }
void Logger::start() { start(BuildOptionsFromConfig()); }

void Logger::start(const Options& opts) {
    std::lock_guard<std::mutex> lk(m_lifecycle_mutex);
    if (m_running.load(std::memory_order_acquire)) return;

    m_slots.clear();
    m_poll_index = 0;
    m_slot_version.fetch_add(1, std::memory_order_release);
    m_generation.fetch_add(1, std::memory_order_release);
    m_nanoseconds_per_tick = NanosecondsPerLogClockTick();
    const auto begin_ticks = detail::ReadLogClockTicks();
    const auto now_sys = std::chrono::system_clock::now();
    const auto end_ticks = detail::ReadLogClockTicks();
    m_base_clock_ticks = begin_ticks + (end_ticks - begin_ticks) / 2;
    m_epoch_ns = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now_sys.time_since_epoch()).count());

    m_file_path = opts.file_path;
    m_per_thread_bytes = std::max<std::size_t>(4096, opts.per_thread_queue_bytes);
    m_flush_interval_ms = std::max<std::size_t>(1, opts.flush_interval_ms);
    m_backend_sleep_duration = std::max(opts.backend_sleep_duration,
                                        std::chrono::nanoseconds::zero());
    m_backend_cpu_affinity = opts.backend_cpu_affinity;
    m_writer_cpu_affinity = opts.writer_cpu_affinity;
    m_max_file_size = std::max<std::size_t>(1, opts.max_file_size);
    m_level.store(opts.level, std::memory_order_release);
    m_dropped_count.store(0, std::memory_order_release);

    m_running.store(true, std::memory_order_release);
    m_fmt_buf.reserve(kWriteBufferReserve);
    m_write_buf.reserve(kWriteBufferReserve);
    m_consumer = std::jthread([this]{ consumerRun(); });
    m_writer = std::jthread([this]{ writeThreadRun(); });
}

void Logger::stop() {
    std::lock_guard<std::mutex> lk(m_lifecycle_mutex);
    if (!m_running.exchange(false, std::memory_order_acq_rel)) return;
    m_consumer_cv.notify_one();
    m_write_cv.notify_one();
    if (m_consumer.joinable()) m_consumer.join();
    if (m_writer.joinable()) m_writer.join();
    if (auto d = m_dropped_count.load(std::memory_order_relaxed))
        std::cerr << "[Logger] total dropped logs=" << d << '\n';
}

void Logger::reloadFromConfig() {
    auto opts = BuildOptionsFromConfig();
    std::lock_guard<std::mutex> lk(m_lifecycle_mutex);
    m_level.store(opts.level, std::memory_order_release);
    m_flush_interval_ms = std::max<std::size_t>(1, opts.flush_interval_ms);
    m_max_file_size = std::max<std::size_t>(1, opts.max_file_size);
    m_file_path = opts.file_path;
}

void Logger::ensureStarted() { if (!m_running.load(std::memory_order_acquire)) start(); }

void Logger::flushAll() {
    if (m_log_fd < 0) return;

    // Try-lock the write mutex — in a signal handler we can't block.
    std::unique_lock<std::mutex> lk(m_write_mutex, std::try_to_lock);

    // Write any buffered data from the format thread's buffer.
    if (!m_fmt_buf.empty()) {
        auto* data = m_fmt_buf.data();
        auto size = m_fmt_buf.size();
        auto written = ::write(m_log_fd, data, size);
        if (written > 0) {
            m_current_file_size += static_cast<std::size_t>(written);
            m_fmt_buf.erase(0, static_cast<std::size_t>(written));
        }
    }

    // Write any pending write-thread buffer.
    if (!m_write_buf.empty()) {
        auto* data = m_write_buf.data();
        auto size = m_write_buf.size();
        auto written = ::write(m_log_fd, data, size);
        if (written > 0) {
            m_current_file_size += static_cast<std::size_t>(written);
            m_write_buf.erase(0, static_cast<std::size_t>(written));
        }
    }

    ::fsync(m_log_fd);
}

void Logger::flush() {
    if (m_running.load(std::memory_order_acquire)) {
        m_need_flush.store(true, std::memory_order_release);
        m_consumer_cv.notify_one();
    }
}
void Logger::setLevel(LogLevel l) noexcept { m_level.store(l, std::memory_order_release); }
LogLevel Logger::level() const noexcept { return m_level.load(std::memory_order_acquire); }
bool Logger::isRunning() const noexcept { return m_running.load(std::memory_order_acquire); }
bool Logger::shouldLog(LogLevel level) {
    return static_cast<std::uint8_t>(level) >=
           static_cast<std::uint8_t>(m_level.load(std::memory_order_relaxed));
}

std::shared_ptr<Logger::ThreadSlot> Logger::registerThisThread() {
    auto slot = std::make_shared<ThreadSlot>();
    slot->queue = std::make_unique<SpscByteQueue>(m_per_thread_bytes);
    slot->tid = std::this_thread::get_id();
    slot->generation = m_generation.load(std::memory_order_acquire);
    std::lock_guard<std::mutex> lk(m_slot_mutex);
    m_slots.push_back(slot);
    m_slot_version.fetch_add(1, std::memory_order_release);
    return slot;
}


void Logger::consumerRun() {
    PinCurrentThread(m_backend_cpu_affinity);
    closeLogFile(); openLogFile();
    std::string& wb = m_fmt_buf;
    std::vector<std::shared_ptr<ThreadSlot>> active_slots;
    std::uint64_t active_slot_version = std::numeric_limits<std::uint64_t>::max();

    auto refreshActiveSlots = [&](bool force = false) {
        const auto version = m_slot_version.load(std::memory_order_acquire);
        if (!force && version == active_slot_version) return;

        std::lock_guard<std::mutex> lk(m_slot_mutex);
        active_slots.assign(m_slots.begin(), m_slots.end());
        active_slot_version = m_slot_version.load(std::memory_order_relaxed);
        if (!active_slots.empty()) m_poll_index %= active_slots.size();
        else m_poll_index = 0;
    };

    // Second-level cache for localtime_r.
    struct {
        std::time_t last_sec{0};
        std::array<char, 20> buf{};
        std::size_t len{0};
    } dtc;

    auto formatLine = [&](const LogEntry& e) {
        constexpr std::uint64_t NS = 1'000'000'000ULL;
        const auto elapsed_ticks = e.timestamp_ticks - m_base_clock_ticks;
        const auto timestamp_ns = m_epoch_ns + static_cast<std::uint64_t>(
            static_cast<double>(elapsed_ticks) * m_nanoseconds_per_tick);
        auto secs = static_cast<std::time_t>(timestamp_ns / NS);
        int ms = static_cast<int>((timestamp_ns % NS) / 1'000'000ULL);
        if (secs != dtc.last_sec) {
            std::tm tm_buf{};
            localtime_r(&secs, &tm_buf);
            auto r = fmt::format_to_n(dtc.buf.data(), dtc.buf.size(),
                "{:04d}-{:02d}-{:02d} {:02d}:{:02d}:{:02d}",
                tm_buf.tm_year+1900, tm_buf.tm_mon+1, tm_buf.tm_mday,
                tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec);
            dtc.len = r.size; dtc.last_sec = secs;
        }
        fmt::format_to(std::back_inserter(wb), "{}.{:03d} [{}] [tid={}]",
            std::string_view(dtc.buf.data(), dtc.len), ms,
            LogLevelToString(e.hasStaticMetadata()
                ? e.metadata->level
                : static_cast<LogLevel>(e.level)), e.thread_id);
        if (e.msgid != 0)
            fmt::format_to(std::back_inserter(wb), " [msgid={}]", e.msgid);
        if (e.method != nullptr)
            fmt::format_to(std::back_inserter(wb), " [method={}]",
                           e.method->full_name());
        wb.push_back(' ');
        e.runFormat(wb);
        wb.push_back('\n');
    };

    auto swapBuffer = [&] {
        std::lock_guard<std::mutex> lk(m_write_mutex);
        if (wb.size() >= kWriteThreshold) {
            m_write_buf.swap(wb);
            m_write_ready = true;
            m_write_cv.notify_one();
        }
    };

    while (m_running.load(std::memory_order_acquire)) {
        refreshActiveSlots();
        std::size_t total = 0;
        std::size_t polled = 0;
        std::size_t idx = m_poll_index;
        while (!active_slots.empty() && polled < active_slots.size() &&
               total < kMaxDequeuePerRound) {
            auto& slp = active_slots[idx];
            if (slp && !slp->dead.load(std::memory_order_relaxed)) {
                while (total < kMaxDequeuePerRound) {
                    auto* entry = slp->queue->front();
                    if (entry == nullptr) break;
                    formatLine(*entry);
                    slp->queue->pop(*entry);
                    ++total;
                }
            }
            idx = (idx + 1) % active_slots.size();
            ++polled;
        }
        m_poll_index = idx;

        if (total > 0) swapBuffer();

        if (total == 0) {
            // Going idle — flush any buffered output.
            {
                std::lock_guard<std::mutex> lk(m_write_mutex);
                if (!wb.empty()) {
                    m_write_buf.swap(wb);
                    m_write_ready = true;
                    m_write_cv.notify_one();
                }
            }

            refreshActiveSlots();
            bool has_data = false;
            for (auto& slp : active_slots) {
                if (slp && !slp->dead.load(std::memory_order_relaxed) &&
                    !slp->queue->empty()) {
                    has_data = true;
                    break;
                }
            }
            if (has_data || !m_running.load(std::memory_order_acquire)) {
                continue;
            }
            if (m_need_flush.load(std::memory_order_acquire)) {
                // Wake the writer even when there is no formatted payload:
                // it owns the fsync and clears m_need_flush.
                {
                    std::lock_guard<std::mutex> lk(m_write_mutex);
                    m_write_ready = true;
                }
                m_write_cv.notify_one();
                continue;
            }

            if (m_backend_sleep_duration == std::chrono::nanoseconds::zero()) {
                cpu_pause();
                continue;
            }

            // Producers never touch a shared notification counter. Polling at
            // a bounded interval keeps their hot path to one SPSC publish,
            // while stop()/flush() can still wake this thread immediately.
            std::unique_lock<std::mutex> cvlk(m_consumer_mutex);
            m_consumer_cv.wait_for(cvlk, m_backend_sleep_duration, [this]{
                return !m_running.load(std::memory_order_acquire) ||
                       m_need_flush.load(std::memory_order_acquire);
            });
        }
    }

    // Final drain
    refreshActiveSlots(true);
    for (auto& slp : active_slots) {
        if (!slp) continue;
        for (;;) {
            std::size_t n = 0;
            while (n < kMaxDequeuePerRound) {
                auto* entry = slp->queue->front();
                if (entry == nullptr) break;
                formatLine(*entry);
                slp->queue->pop(*entry);
                ++n;
            }
            if (n == 0) break;
        }
    }
    {
        std::lock_guard<std::mutex> lk(m_write_mutex);
        if (!wb.empty()) {
            m_write_buf.swap(wb);
            m_write_ready = true;
        }
        m_write_cv.notify_one();
    }
}

void Logger::writeThreadRun() {
    PinCurrentThread(m_writer_cpu_affinity);
    std::string buf;
    buf.reserve(kWriteBufferReserve);
    auto last_flush = std::chrono::steady_clock::now();

    while (m_running.load(std::memory_order_acquire) || m_write_ready) {
        {
            std::unique_lock<std::mutex> lk(m_write_mutex);
            m_write_cv.wait(lk, [this]{
                return m_write_ready || !m_running.load(std::memory_order_acquire);
            });
            if (m_write_ready) {
                buf.swap(m_write_buf);
                m_write_ready = false;
            } else if (!m_running.load(std::memory_order_acquire)) {
                break;
            }
        }

        if (!buf.empty()) {
            if (m_log_fd >= 0) [[likely]] {
                rotateIfNeeded();
                auto* data = buf.data();
                auto size = buf.size();
                auto written = ::write(m_log_fd, data, size);
                if (written > 0) m_current_file_size += static_cast<std::size_t>(written);
            } else std::cerr.write(buf.data(), static_cast<std::streamsize>(buf.size()));
            buf.clear();
        }

        bool f = m_need_flush.exchange(false, std::memory_order_acq_rel);
        auto now = std::chrono::steady_clock::now();
        bool tf = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_flush).count()
                  >= static_cast<long long>(m_flush_interval_ms);
        if ((f || tf) && m_log_fd >= 0) [[likely]] { fsync(m_log_fd); last_flush = now; }
    }

    // Final flush
    if (!buf.empty() && m_log_fd >= 0) {
        ::write(m_log_fd, buf.data(), buf.size());
        fsync(m_log_fd);
    }
    closeLogFile();
}

// ── Crash handler ───────────────────────────────────────────────────
namespace {
std::atomic<int> g_crash_installed{0};
} // namespace

extern "C" void rocketCrashHandler(int sig) {
    rocket::Logger::getInstance().flushAll();
    ::signal(sig, SIG_DFL);
    ::raise(sig);
}

namespace {
void atexitFlush() { rocket::Logger::getInstance().flushAll(); }
} // namespace

void Logger::installCrashHandler() {
    if (g_crash_installed.exchange(1)) return; // once

    struct sigaction sa{};
    sa.sa_handler = rocketCrashHandler;
    sigemptyset(&sa.sa_mask);

    for (int sig : {SIGSEGV, SIGABRT, SIGBUS, SIGFPE, SIGILL}) {
        sigaction(sig, &sa, nullptr);
    }
    std::atexit(atexitFlush);
}

void Logger::openLogFile() {
    std::error_code ec;
    auto p = m_file_path.parent_path();
    if (!p.empty()) std::filesystem::create_directories(p, ec);
    m_log_fd = ::open(m_file_path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (m_log_fd >= 0 && std::filesystem::exists(m_file_path, ec))
        m_current_file_size = std::filesystem::file_size(m_file_path, ec);
}
void Logger::closeLogFile() {
    if (m_log_fd >= 0) { fsync(m_log_fd); ::close(m_log_fd); m_log_fd = -1; }
}
void Logger::rotateIfNeeded() {
    if (m_max_file_size == 0 || m_current_file_size < m_max_file_size) return;
    closeLogFile();
    auto ts = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    std::error_code ec;
    std::filesystem::rename(m_file_path, m_file_path.string() + "." + std::to_string(ts), ec);
    openLogFile();
}

} // namespace rocket
