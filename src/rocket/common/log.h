#pragma once

#include "rocket/common/runtime.h"
#include "rocket/common/singleton.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fmt/base.h>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <deque>

#include <fmt/format.h>
#include <fmt/std.h>

#if defined(__linux__)
#include <sys/syscall.h>
#include <unistd.h>
#elif defined(_WIN32)
#include <windows.h>
#elif defined(__APPLE__)
#include <pthread.h>
#endif

#if defined(__x86_64__) || defined(__i386__)
#include <x86intrin.h>
#endif

namespace rocket {

namespace detail {

// Keep timestamp capture on the producer path as cheap as the queue write.
// Conversion to wall-clock nanoseconds happens on the consumer thread.
[[nodiscard]] inline std::uint64_t ReadLogClockTicks() noexcept {
#if defined(__x86_64__) || defined(__i386__)
    return __rdtsc();
#elif defined(__aarch64__)
    std::uint64_t ticks{0};
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(ticks));
    return ticks;
#else
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
#endif
}

} // namespace detail

#ifndef ROCKET_MIN_LOG_LEVEL
#ifdef NDEBUG
#define ROCKET_MIN_LOG_LEVEL 1   // release: strip DEBUG
#else
#define ROCKET_MIN_LOG_LEVEL 0   // debug: keep all
#endif
#endif

enum class LogLevel : std::uint8_t { Debug = 0, Info = 1, Warn = 2, Error = 3 };

[[nodiscard]] constexpr std::string_view LogLevelToString(LogLevel level) {
    switch (level) {
    case LogLevel::Debug: return "DEBUG";
    case LogLevel::Info:  return "INFO";
    case LogLevel::Warn:  return "WARN";
    case LogLevel::Error: return "ERROR";
    }
    return "UNKNOWN";
}

class Logger final : public Singleton<Logger> {
  public:
    static constexpr std::size_t kDefaultQueueCapacity = 1U << 14;
    // Queue capacity in *bytes* (not entries), Quill-style.
    // 256 KB ≅ 3500 entries @ 72 B/entry — plenty for burst absorption.
    static constexpr std::size_t kPerThreadQueueBytes = 256 * 1024;
    static constexpr std::size_t kDefaultFlushIntervalMs = 50;
    static constexpr std::chrono::nanoseconds kDefaultBackendSleepDuration{100'000};
    static constexpr std::size_t kDefaultMaxFileSize = 1024ULL * 1024ULL * 1024ULL;

    struct LogMetadata {
        using FormatFn = void (*)(std::string&, fmt::string_view, const char*);
        fmt::string_view format;
        LogLevel level;
        FormatFn format_fn;
    };

    struct LogEntry {
        // Per-arg codec: encode arguments with type tag + compact value inline.
        // Consumer reconstructs fmt::format_args via a switch.  No fmt internals.
        static constexpr std::size_t kFmtStorageSize = 80;

        enum class ArgType : std::uint8_t {
            Int = 0, Uint, Int64, Uint64, Double, LongDouble, Bool,
            Char, String, Ptr
        };

        template <typename... Args>
        void setArgs(fmt::string_view fmt, Args&&... args) {
            has_static_metadata = false;
            fmt_ptr = fmt.data();
            fmt_len = static_cast<std::uint16_t>(fmt.size());
            encodeArgs(std::forward<Args>(args)...);
        }

        template <typename... Args>
        void setArgs(const LogMetadata* static_metadata, Args&&... args) {
            has_static_metadata = true;
            metadata = static_metadata;
            encodeStaticArgs(std::forward<Args>(args)...);
        }

        void runFormat(std::string& out) {
            const auto format = has_static_metadata
                ? metadata->format
                : fmt::string_view(fmt_ptr, fmt_len);
            if (num_args == 0) {
                out.append(format.data(), format.size());
                return;
            }
            auto* data = heap_allocated ? arg_data : fmt_data;
            const int n = static_cast<int>(num_args);
            if (has_static_metadata) {
                metadata->format_fn(out, format, data);
            } else {
                const auto* tags = reinterpret_cast<const uint8_t*>(data);
                const char* p = alignPtr(data + n, 8);

                fmt::basic_format_arg<fmt::format_context> decoded[15];
                for (int i = 0; i < n; ++i)
                    p = decodeOneArg(p, static_cast<ArgType>(tags[i]), decoded[i]);

                fmt::basic_format_args<fmt::format_context> fargs(decoded, n);
                fmt::vformat_to(std::back_inserter(out), format, fargs);
            }

            if (heap_allocated) {
                delete[] arg_data;
                arg_data = fmt_data;
                heap_allocated = false;
            }
        }

        template <typename... Args>
        static void runStaticFormat(std::string& out, fmt::string_view format,
                                    const char* data) {
            constexpr std::size_t count = sizeof...(Args);
            std::array<fmt::basic_format_arg<fmt::format_context>, count> decoded;
            const char* p = data;
            std::size_t index = 0;
            ((p = decodeTypedArg<Args>(p, decoded[index++])), ...);
            fmt::basic_format_args<fmt::format_context> fargs(decoded.data(),
                                                               count);
            fmt::vformat_to(std::back_inserter(out), format, fargs);
        }

      private:
        static const char* alignPtr(const char* p, int a) {
            auto v = reinterpret_cast<uintptr_t>(p);
            return reinterpret_cast<const char*>((v + a - 1) & ~static_cast<uintptr_t>(a - 1));
        }
        static char* alignPtr(char* p, int a) {
            auto v = reinterpret_cast<uintptr_t>(p);
            return reinterpret_cast<char*>((v + a - 1) & ~static_cast<uintptr_t>(a - 1));
        }

        template <typename>
        static constexpr bool kAlwaysFalse = false;

        template <typename T>
        static constexpr bool kCharArray =
            std::is_array_v<std::remove_cvref_t<T>> &&
            std::is_same_v<std::remove_cv_t<
                std::remove_extent_t<std::remove_cvref_t<T>>>, char>;

        template <typename T>
        static constexpr bool kCharPointer =
            std::is_pointer_v<std::remove_cvref_t<T>> &&
            std::is_same_v<std::remove_cv_t<std::remove_pointer_t<
                std::remove_cvref_t<T>>>, char>;

        template <typename T>
        static constexpr bool kStringLike =
            kCharArray<T> || kCharPointer<T> ||
            std::is_same_v<std::remove_cvref_t<T>, std::string> ||
            std::is_same_v<std::remove_cvref_t<T>, std::string_view> ||
            std::is_same_v<std::remove_cvref_t<T>, fmt::string_view>;

        template <typename T>
        static constexpr ArgType argType() {
            using U = std::remove_cvref_t<T>;
            if constexpr (std::is_same_v<U, bool>) {
                return ArgType::Bool;
            } else if constexpr (std::is_same_v<U, char>) {
                return ArgType::Char;
            } else if constexpr (std::is_enum_v<U>) {
                return argType<std::underlying_type_t<U>>();
            } else if constexpr (std::is_integral_v<U> && std::is_signed_v<U>) {
                return sizeof(U) <= sizeof(int) ? ArgType::Int : ArgType::Int64;
            } else if constexpr (std::is_integral_v<U> && std::is_unsigned_v<U>) {
                return sizeof(U) <= sizeof(unsigned) ? ArgType::Uint : ArgType::Uint64;
            } else if constexpr (std::is_same_v<U, long double>) {
                return ArgType::LongDouble;
            } else if constexpr (std::is_floating_point_v<U>) {
                return ArgType::Double;
            } else if constexpr (kStringLike<U>) {
                return ArgType::String;
            } else if constexpr (std::is_same_v<U, std::thread::id>) {
                // The log prefix already carries the native TID. Encode an
                // explicitly logged std::thread::id as its stable hash rather
                // than retaining a fmt custom-type handle into producer memory.
                return ArgType::Uint64;
            } else if constexpr (std::is_pointer_v<U> ||
                                 std::is_same_v<U, std::nullptr_t>) {
                return ArgType::Ptr;
            } else {
                static_assert(kAlwaysFalse<U>, "unsupported async log argument type");
            }
        }

        template <typename T>
        static std::string_view asStringView(const T& value) {
            using U = std::remove_cvref_t<T>;
            if constexpr (kCharArray<U>) {
                return std::string_view(value, std::char_traits<char>::length(value));
            } else if constexpr (kCharPointer<U>) {
                return value == nullptr ? std::string_view{} : std::string_view(value);
            } else if constexpr (std::is_same_v<U, std::string>) {
                return std::string_view(value);
            } else if constexpr (std::is_same_v<U, std::string_view>) {
                return value;
            } else {
                return std::string_view(value.data(), value.size());
            }
        }

        template <typename T>
        static std::size_t encodedSize(const T& value) {
            using U = std::remove_cvref_t<T>;
            if constexpr (std::is_enum_v<U>) {
                return encodedSize(static_cast<std::underlying_type_t<U>>(value));
            } else if constexpr (kStringLike<U>) {
                return sizeof(std::size_t) + asStringView(value).size();
            } else if constexpr (std::is_same_v<U, bool> ||
                                 std::is_same_v<U, char>) {
                return 1;
            } else if constexpr (std::is_integral_v<U>) {
                return sizeof(U) <= sizeof(int) ? sizeof(int) : sizeof(std::uint64_t);
            } else if constexpr (std::is_same_v<U, long double>) {
                return sizeof(long double);
            } else if constexpr (std::is_floating_point_v<U>) {
                return sizeof(double);
            } else {
                return sizeof(std::uint64_t);
            }
        }

        template <typename T>
        static void encodeOneArg(char*& p, T&& value) {
            using U = std::remove_cvref_t<T>;
            if constexpr (std::is_same_v<U, bool>) {
                *p++ = value ? 1 : 0;
            } else if constexpr (std::is_same_v<U, char>) {
                *p++ = value;
            } else if constexpr (std::is_enum_v<U>) {
                encodeOneArg(p, static_cast<std::underlying_type_t<U>>(value));
            } else if constexpr (std::is_integral_v<U> && std::is_signed_v<U>) {
                if constexpr (sizeof(U) <= sizeof(int)) {
                    const int encoded = static_cast<int>(value);
                    memcpy(p, &encoded, sizeof(encoded)); p += sizeof(encoded);
                } else {
                    const std::int64_t encoded = static_cast<std::int64_t>(value);
                    memcpy(p, &encoded, sizeof(encoded)); p += sizeof(encoded);
                }
            } else if constexpr (std::is_integral_v<U> && std::is_unsigned_v<U>) {
                if constexpr (sizeof(U) <= sizeof(unsigned)) {
                    const unsigned encoded = static_cast<unsigned>(value);
                    memcpy(p, &encoded, sizeof(encoded)); p += sizeof(encoded);
                } else {
                    const std::uint64_t encoded = static_cast<std::uint64_t>(value);
                    memcpy(p, &encoded, sizeof(encoded)); p += sizeof(encoded);
                }
            } else if constexpr (std::is_same_v<U, long double>) {
                memcpy(p, &value, sizeof(value)); p += sizeof(value);
            } else if constexpr (std::is_floating_point_v<U>) {
                const double encoded = static_cast<double>(value);
                memcpy(p, &encoded, sizeof(encoded)); p += sizeof(encoded);
            } else if constexpr (kStringLike<U>) {
                const auto view = asStringView(value);
                const std::size_t size = view.size();
                memcpy(p, &size, sizeof(size)); p += sizeof(size);
                if (size != 0) memcpy(p, view.data(), size);
                p += size;
            } else if constexpr (std::is_same_v<U, std::thread::id>) {
                const auto encoded = static_cast<std::uint64_t>(
                    std::hash<std::thread::id>{}(value));
                memcpy(p, &encoded, sizeof(encoded)); p += sizeof(encoded);
            } else if constexpr (std::is_same_v<U, std::nullptr_t>) {
                const void* encoded = nullptr;
                memcpy(p, &encoded, sizeof(encoded)); p += sizeof(encoded);
            } else if constexpr (std::is_pointer_v<U>) {
                const void* encoded = reinterpret_cast<const void*>(value);
                memcpy(p, &encoded, sizeof(encoded)); p += sizeof(encoded);
            } else {
                static_assert(kAlwaysFalse<U>, "unsupported async log argument type");
            }
        }

        template <typename... Args>
        void encodeArgs(Args&&... args) {
            constexpr std::size_t count = sizeof...(Args);
            static_assert(count <= 15, "async logger supports at most 15 arguments");
            num_args = static_cast<std::uint8_t>(count);
            heap_allocated = false;
            arg_data = fmt_data;
            if constexpr (count == 0) return;

            std::size_t total = count;
            total = (total + 7U) & ~std::size_t{7U};
            ((total += encodedSize(args)), ...);

            char* dst = fmt_data;
            if (total > kFmtStorageSize) {
                heap_allocated = true;
                dst = new char[total];
                arg_data = dst;
            }

            const std::array<ArgType, count> tags{argType<Args>()...};
            for (std::size_t i = 0; i < count; ++i)
                dst[i] = static_cast<char>(tags[i]);
            char* p = alignPtr(dst + count, 8);
            (encodeOneArg(p, std::forward<Args>(args)), ...);
        }

        template <typename... Args>
        void encodeStaticArgs(Args&&... args) {
            constexpr std::size_t count = sizeof...(Args);
            static_assert(count <= 15, "async logger supports at most 15 arguments");
            num_args = static_cast<std::uint8_t>(count);
            heap_allocated = false;
            arg_data = fmt_data;
            if constexpr (count == 0) return;

            std::size_t total = 0;
            ((total += encodedSize(args)), ...);
            char* dst = fmt_data;
            if (total > kFmtStorageSize) {
                heap_allocated = true;
                dst = new char[total];
                arg_data = dst;
            }
            char* p = dst;
            (encodeOneArg(p, std::forward<Args>(args)), ...);
        }

        template <typename T>
        static const char* decodeTypedArg(
            const char* p,
            fmt::basic_format_arg<fmt::format_context>& out) {
            return decodeOneArg(p, argType<T>(), out);
        }

        static const char* decodeOneArg(const char* p, ArgType tag,
                                        fmt::basic_format_arg<fmt::format_context>& out) {
            switch (tag) {
            case ArgType::Int:    { int v; memcpy(&v, p, 4); p += 4; out = farg(v); break; }
            case ArgType::Uint:   { unsigned v; memcpy(&v, p, 4); p += 4; out = farg(v); break; }
            case ArgType::Int64:  { long long v; memcpy(&v, p, 8); p += 8; out = farg(v); break; }
            case ArgType::Uint64: { unsigned long long v; memcpy(&v, p, 8); p += 8; out = farg(v); break; }
            case ArgType::Double: { double v; memcpy(&v, p, 8); p += 8; out = farg(v); break; }
            case ArgType::LongDouble: { long double v; memcpy(&v, p, sizeof(v)); p += sizeof(v); out = farg(v); break; }
            case ArgType::Bool:   { bool v = *p++; out = farg(v); break; }
            case ArgType::Char:   { char v = *p++; out = farg(v); break; }
            case ArgType::String: {
                std::size_t size; memcpy(&size, p, sizeof(size)); p += sizeof(size);
                out = farg(fmt::string_view(p, size)); p += size; break;
            }
            case ArgType::Ptr:    { const void* v; memcpy(&v, p, 8); p += 8; out = farg(v); break; }
            default:              { const void* v; memcpy(&v, p, 8); p += 8; out = farg(v); break; }
            }
            return p;
        }

        template <typename T>
        static fmt::basic_format_arg<fmt::format_context> farg(T&& v) {
            return fmt::basic_format_arg<fmt::format_context>(static_cast<T&&>(v));
        }

      public:
        union {
            const LogMetadata* metadata{nullptr};
            const char* fmt_ptr;
        };
        std::uint64_t thread_id{0};
        std::uint64_t timestamp_ticks{0};
        std::uint8_t level{0};
        // Numeric message IDs stay allocation-free in the async log record.
        std::uint64_t msgid{0};
        std::string method_name;
        std::uint8_t num_args{0};
        bool heap_allocated{false};
        bool has_static_metadata{false};
        std::uint16_t fmt_len{0};
        char* arg_data{fmt_data};
        alignas(std::max_align_t) char fmt_data[kFmtStorageSize];
    };

    template <typename... Args>
    static constexpr LogMetadata makeLogMetadata(fmt::string_view format,
                                                  LogLevel level) {
        return LogMetadata{format, level,
                           &LogEntry::template runStaticFormat<Args...>};
    }

    struct Options {
        std::filesystem::path file_path{"./rocket_rpc.log"};
        std::size_t per_thread_queue_bytes{kPerThreadQueueBytes};
        std::size_t flush_interval_ms{kDefaultFlushIntervalMs};
        // How long the backend waits between empty queue polls. Zero enables
        // busy polling for latency benchmarks and dedicated logging cores.
        std::chrono::nanoseconds backend_sleep_duration{kDefaultBackendSleepDuration};
        // Linux-only optional CPU affinity. Negative values leave scheduling
        // to the OS, which remains the production default.
        int backend_cpu_affinity{-1};
        int writer_cpu_affinity{-1};
        std::size_t max_file_size{kDefaultMaxFileSize};
        LogLevel level{LogLevel::Debug};
    };

    void start();
    void start(const Options& options);
    void stop();
    void reloadFromConfig();
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;
    Logger(Logger&&) = delete;
    Logger& operator=(Logger&&) = delete;

    template <typename... Args>
    void log(LogLevel level, fmt::format_string<Args...> fmt, Args&&... args);

    template <typename... Args>
    void log(const LogMetadata* metadata, fmt::format_string<Args...> fmt,
             Args&&... args);

    void flush();

    // Emergency flush: attempt to write all buffered data to disk.
    // Async-signal-safe-ish: avoids heap allocation, uses try_lock.
    // Call from signal handlers (SIGSEGV/SIGABRT) or atexit hooks.
    void flushAll();

    // Install signal handlers that call flushAll() on SIGSEGV/SIGABRT/SIGBUS/SIGFPE.
    // Registers with atexit as well.  Idempotent (install once).
    static void installCrashHandler();

    void setLevel(LogLevel level) noexcept;
    [[nodiscard]] LogLevel level() const noexcept;
    [[nodiscard]] bool isRunning() const noexcept;
    [[nodiscard]] bool shouldLog(LogLevel level);
    [[nodiscard]] std::uint64_t getDroppedCount() const noexcept {
        return m_dropped_count.load(std::memory_order_relaxed);
    }

  private:
    // ── SPSC bounded ring queue (one per thread, single producer, no CAS) ──
    template <typename T>
    class SpscBoundedQueue {
      public:
        static constexpr std::size_t kCacheLine = 64;
        explicit SpscBoundedQueue(std::size_t cap);
        [[nodiscard]] T* tryClaim();
        void publish();
        [[nodiscard]] bool tryDequeue(T& out);
        [[nodiscard]] bool empty() const noexcept;
      private:
        struct alignas(kCacheLine) Cell {
            std::atomic<std::size_t> sequence;
            T storage;
            Cell() noexcept : sequence(0), storage() {}
            Cell(Cell&&) = delete;
        };
        std::size_t m_mask, m_capacity;
        std::unique_ptr<Cell[]> m_buffer;
        std::size_t m_pos{0};              // producer only
        alignas(kCacheLine) std::size_t m_dq{0};  // consumer only, no atomic needed
    };

    // ── Per-thread state ────────────────────────────────────────────────
    struct ThreadSlot {
        std::unique_ptr<SpscBoundedQueue<LogEntry>> queue;
        std::thread::id tid;
        std::uint64_t generation{0};
        std::atomic<bool> dead{false};
    };

    friend class Singleton<Logger>;
    Logger() = default;
    ~Logger();
    void consumerRun();
    void writeThreadRun();
    void ensureStarted();
    std::shared_ptr<ThreadSlot> registerThisThread();
    void openLogFile();
    void closeLogFile();
    void rotateIfNeeded();

    template <typename... Args>
    void enqueue(LogLevel level, const LogMetadata* metadata,
                 fmt::string_view format, Args&&... args);

    static constexpr std::size_t kMaxDequeuePerRound = 1024;       // ~150KB per round
    static constexpr std::size_t kWriteThreshold = 256ULL * 1024;  // batch to this size before ::write
    static constexpr std::size_t kWriteBufferReserve = 256ULL * 1024;
    // config
    std::size_t m_per_thread_bytes{kPerThreadQueueBytes};
    std::filesystem::path m_file_path;
    std::size_t m_flush_interval_ms{kDefaultFlushIntervalMs};
    std::chrono::nanoseconds m_backend_sleep_duration{kDefaultBackendSleepDuration};
    int m_backend_cpu_affinity{-1};
    int m_writer_cpu_affinity{-1};
    std::size_t m_max_file_size{kDefaultMaxFileSize};
    std::atomic<bool> m_running{false};
    std::atomic<LogLevel> m_level{LogLevel::Debug};
    std::atomic<bool> m_need_flush{false};

    // clock calibration
    std::uint64_t m_epoch_ns{0};
    std::uint64_t m_base_clock_ticks{0};
    double m_nanoseconds_per_tick{1.0};

    // sync (destroyed AFTER thread/queues)
    std::mutex m_lifecycle_mutex;
    std::condition_variable m_consumer_cv;
    std::mutex m_consumer_mutex;

    // per-thread queue registry
    std::atomic<std::uint64_t> m_generation{0};
    std::atomic<std::uint64_t> m_slot_version{0};
    std::mutex m_slot_mutex;
    std::deque<std::shared_ptr<ThreadSlot>> m_slots;
    std::size_t m_poll_index{0};

    // consumer (format thread)
    std::jthread m_consumer;

    // write thread (I/O, separate from format thread)
    std::jthread m_writer;
    std::string m_fmt_buf; // format thread writes here
    std::string m_write_buf; // handed to write thread
    std::mutex m_write_mutex;
    std::condition_variable m_write_cv;
    bool m_write_ready{false};

    // file (raw fd — no stdio overhead)
    int m_log_fd{-1};
    std::size_t m_current_file_size{0};

    // counters
    std::atomic<std::uint64_t> m_dropped_count{0};
};

template <typename... Args>
void Logger::log(LogLevel level, fmt::format_string<Args...> fmt, Args&&... args) {
    enqueue(level, nullptr, fmt.str, std::forward<Args>(args)...);
}

template <typename... Args>
void Logger::log(const LogMetadata* metadata, fmt::format_string<Args...> fmt,
                 Args&&... args) {
    enqueue(metadata->level, metadata, fmt.str, std::forward<Args>(args)...);
}

template <typename... Args>
void Logger::enqueue(LogLevel level, const LogMetadata* metadata,
                     fmt::string_view format, Args&&... args) {
    if (static_cast<std::uint8_t>(level) < static_cast<std::uint8_t>(m_level.load(std::memory_order_relaxed)))
        return;
    if (!m_running.load(std::memory_order_acquire)) [[unlikely]] {
        ensureStarted();
        // start() loads the configured level, which may be stricter than the
        // construction default used by the first shouldLog() check.
        if (static_cast<std::uint8_t>(level) <
            static_cast<std::uint8_t>(m_level.load(std::memory_order_relaxed)))
            return;
    }

    static thread_local std::uint64_t cached_tid = []() {
#if defined(__linux__)
        return static_cast<std::uint64_t>(::syscall(SYS_gettid));
#elif defined(_WIN32)
        return static_cast<std::uint64_t>(::GetCurrentThreadId());
#elif defined(__APPLE__)
        std::uint64_t tid{0}; ::pthread_threadid_np(nullptr, &tid); return tid;
#else
        return std::hash<std::thread::id>{}(std::this_thread::get_id());
#endif
    }();

    const std::uint64_t timestamp_ticks = detail::ReadLogClockTicks();

    // ── lazy-init per-thread SPSC queue ──────────────────────────────────
    static thread_local std::shared_ptr<ThreadSlot> t_slot;
    if (!t_slot || t_slot->generation != m_generation.load(std::memory_order_acquire)) [[unlikely]]
        t_slot = registerThisThread();

    auto slot = t_slot->queue->tryClaim();
    if (!slot) [[unlikely]] {
        m_dropped_count.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    // ── capture metadata ───────────────────────────────────────────────
    auto& entry = *slot;
    entry.thread_id = cached_tid;
    entry.timestamp_ticks = timestamp_ticks;
    entry.level = static_cast<std::uint8_t>(level);
    auto* rt = RunTime::GetRunTime();
    entry.msgid = rt->m_msgid;
    entry.method_name = rt->m_method_name;

    // ── deferred format: consumer calls this to emit the user message ───
    if (metadata != nullptr) {
        entry.setArgs(metadata, std::forward<Args>(args)...);
    } else {
        entry.setArgs(format, std::forward<Args>(args)...);
    }

    t_slot->queue->publish();
}

} // namespace rocket

#define ROCKET_LOG_IMPL(log_level, format, ...)                                               \
    do {                                                                                      \
        auto& rocket_log_instance = ::rocket::Logger::getInstance();                          \
        if (rocket_log_instance.shouldLog(log_level)) {                                       \
            [&]<typename... RocketLogArgs>(RocketLogArgs&&... rocket_log_args) {               \
                static constexpr auto rocket_log_metadata =                                   \
                    ::rocket::Logger::makeLogMetadata<RocketLogArgs...>(format, log_level);    \
                rocket_log_instance.log(                                                      \
                    &rocket_log_metadata, format,                                              \
                    std::forward<RocketLogArgs>(rocket_log_args)...);                          \
            }(__VA_ARGS__);                                                                   \
        }                                                                                     \
    } while (false)

#if ROCKET_MIN_LOG_LEVEL <= 0
#define ROCKET_LOG_DEBUG(format, ...)                                                          \
    ROCKET_LOG_IMPL(::rocket::LogLevel::Debug, format, ##__VA_ARGS__)
#else
#define ROCKET_LOG_DEBUG(format, ...) (void)0
#endif
#if ROCKET_MIN_LOG_LEVEL <= 1
#define ROCKET_LOG_INFO(format, ...)                                                           \
    ROCKET_LOG_IMPL(::rocket::LogLevel::Info, format, ##__VA_ARGS__)
#else
#define ROCKET_LOG_INFO(format, ...) (void)0
#endif
#if ROCKET_MIN_LOG_LEVEL <= 2
#define ROCKET_LOG_WARN(format, ...)                                                           \
    ROCKET_LOG_IMPL(::rocket::LogLevel::Warn, format, ##__VA_ARGS__)
#else
#define ROCKET_LOG_WARN(format, ...) (void)0
#endif
#if ROCKET_MIN_LOG_LEVEL <= 3
#define ROCKET_LOG_ERROR(format, ...)                                                          \
    ROCKET_LOG_IMPL(::rocket::LogLevel::Error, format, ##__VA_ARGS__)
#else
#define ROCKET_LOG_ERROR(format, ...) (void)0
#endif
