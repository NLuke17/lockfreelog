// lockfree_logger.hpp — a lock-free, zero-allocation logging engine (C++20, header-only).
//
//   lockfree::Logger log(stderr);
//   LOG(log, "user {} logged in from {}", user_id, ip);   // format checked at compile time
//
// - Wait-free producer: LOG() formats directly into a preallocated ring slot and
//   publishes with a single release-store. No lock, no heap allocation, never blocks.
// - Background thread drains the ring to the sink (FILE*), keeping I/O off the hot path.
// - Single-producer / single-consumer: each index has exactly one writer, so no CAS.
// - Format strings are parsed and arity-checked at COMPILE TIME (a wrong-argument-count
//   LOG is a compile error, not a runtime bug — and can't be a format-string exploit).
//
// Requires C++20 (class-type NTTPs, consteval, if constexpr).

#pragma once

#include <atomic>
#include <thread>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <charconv>
#include <string_view>
#include <type_traits>

namespace lockfree {

// Cache line for false-sharing avoidance. 128 on Apple Silicon (sysctl hw.cachelinesize);
// 64 on most x86. std::hardware_destructive_interference_size is flaky across compilers,
// so we pick the larger, safe value explicitly.
inline constexpr std::size_t kCacheLine = 128;

// ---------------------------------------------------------------------------
// FixedString<N> — a "structural" type so a string literal can be a non-type
// template parameter (C++20). This is what carries a format string into the
// type system so it can be parsed at compile time.
// ---------------------------------------------------------------------------
template <std::size_t N>
struct FixedString {
    char data[N]{};
    consteval FixedString(const char (&s)[N]) {
        for (std::size_t i = 0; i < N; ++i) data[i] = s[i];
    }
    constexpr std::string_view view() const { return {data, N - 1}; }  // drop trailing '\0'

    consteval std::size_t placeholders() const {           // count "{}" at compile time
        std::size_t count = 0;
        for (std::size_t i = 0; i + 1 < N; ++i)
            if (data[i] == '{' && data[i + 1] == '}') { ++count; ++i; }
        return count;
    }
};

// ---------------------------------------------------------------------------
// Formatting: write one argument into [p, end); return the new write pointer.
// if constexpr picks the path per type at compile time — no runtime branching.
// ---------------------------------------------------------------------------
template <class T>
inline char* emit_arg(char* p, char* end, const T& arg) {
    if constexpr (std::is_integral_v<T>) {
        auto res = std::to_chars(p, end, arg);   // allocation-free, locale-free
        return res.ptr;                          // == p if it didn't fit; bounded
    } else {
        const char* s = arg;                     // const char* / char[] decays
        std::size_t n = std::strlen(s);
        if (n > static_cast<std::size_t>(end - p)) n = static_cast<std::size_t>(end - p);
        std::memcpy(p, s, n);
        return p + n;
    }
}

// base case: no args left — copy the remaining literal (bounded).
inline char* format_into(char* p, char* end, std::string_view fmt) {
    std::size_t n = fmt.size();
    if (n > static_cast<std::size_t>(end - p)) n = static_cast<std::size_t>(end - p);
    std::memcpy(p, fmt.data(), n);
    return p + n;
}

// recursive case: copy literal up to "{}", emit one arg, recurse on the rest.
template <class T, class... Rest>
inline char* format_into(char* p, char* end, std::string_view fmt,
                         const T& arg, const Rest&... rest) {
    std::size_t pos = fmt.find("{}");
    std::size_t chunk = (pos == std::string_view::npos) ? fmt.size() : pos;
    if (chunk > static_cast<std::size_t>(end - p)) chunk = static_cast<std::size_t>(end - p);
    std::memcpy(p, fmt.data(), chunk);
    p += chunk;
    if (pos == std::string_view::npos) return p;   // more args than {} — ignore extras
    p = emit_arg(p, end, arg);
    return format_into(p, end, fmt.substr(pos + 2), rest...);
}

// ---------------------------------------------------------------------------
// Logger — the engine. Fixed, preallocated ring + background flush thread.
//   Capacity and max message length are compile-time constants (no heap, ever).
// ---------------------------------------------------------------------------
template <std::size_t Capacity = 8192, std::size_t MaxMsgLen = 256>
class Logger {
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be a power of two");
public:
    // sink is a FILE* the caller owns (stderr, stdout, or an fopen'd file).
    explicit Logger(std::FILE* sink) : sink_(sink) {
        consumer_ = std::thread([this] { drain_loop(); });
    }
    ~Logger() {
        done_.store(true, std::memory_order_relaxed);
        if (consumer_.joinable()) consumer_.join();
    }
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    // The producer hot path. Wait-free: fixed steps, never blocks. Formats directly
    // into the ring slot. Returns false if the buffer is full (message dropped).
    template <FixedString Fmt, class... Args>
    bool log(const Args&... args) {
        static_assert(Fmt.placeholders() == sizeof...(Args),
                      "LOG: number of {} placeholders must match number of arguments");

        std::uint64_t h = head_.load(std::memory_order_relaxed);   // we own head_
        std::uint64_t t = tail_.load(std::memory_order_acquire);   // see freed slots
        if (h - t == Capacity) {                                   // full -> drop
            dropped_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        Slot& s = buf_[h & (Capacity - 1)];
        char* p = format_into(s.data, s.data + MaxMsgLen, Fmt.view(), args...);
        s.len = static_cast<std::uint16_t>(p - s.data);
        head_.store(h + 1, std::memory_order_release);             // publish
        return true;
    }

    std::uint64_t dropped() const { return dropped_.load(std::memory_order_relaxed); }

private:
    struct Slot { char data[MaxMsgLen]; std::uint16_t len = 0; };

    void drain_loop() {
        // Coalesce a whole drain pass into one fwrite instead of one syscall per
        // message — keeps the consumer fast enough to sustain the producer's rate.
        static constexpr std::size_t kOutBuf = 64 * 1024;
        char out[kOutBuf];
        std::uint64_t t = tail_.load(std::memory_order_relaxed);   // we own tail_
        for (;;) {
            std::uint64_t h = head_.load(std::memory_order_acquire);
            if (t == h) {                                          // empty
                if (done_.load(std::memory_order_relaxed)) {
                    tail_.store(t, std::memory_order_release);
                    return;                                        // drain-then-exit
                }
                std::this_thread::yield();
                continue;
            }
            std::size_t used = 0;
            while (t != h) {                                       // drain the batch
                const Slot& s = buf_[t & (Capacity - 1)];
                if (used + s.len + 1 > kOutBuf) {                  // flush if full
                    std::fwrite(out, 1, used, sink_);
                    used = 0;
                }
                std::memcpy(out + used, s.data, s.len);
                used += s.len;
                out[used++] = '\n';
                ++t;
            }
            if (used) std::fwrite(out, 1, used, sink_);
            tail_.store(t, std::memory_order_release);             // publish freed slots
        }
    }

    // The two hot indices, each on its own cache line to avoid false sharing.
    alignas(kCacheLine) std::atomic<std::uint64_t> head_{0};   // producer writes
    alignas(kCacheLine) std::atomic<std::uint64_t> tail_{0};   // consumer writes

    std::array<Slot, Capacity> buf_{};      // the whole ring, preallocated inline
    std::atomic<std::uint64_t> dropped_{0};
    std::atomic<bool>          done_{false};
    std::FILE*                 sink_;
    std::thread                consumer_;
};

}  // namespace lockfree

// Convenience macro: LOG(logger, "fmt {}", arg) -> logger.log<"fmt {}">(arg).
// The format string is a compile-time template argument (see Logger::log).
#define LOG(logger, fmt, ...) (logger).template log<fmt>(__VA_ARGS__)
