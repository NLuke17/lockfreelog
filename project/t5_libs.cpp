// Tier 5 (part 2) — Benchmark our lock-free logger vs printf / iostream / fmt / spdlog.
//
// Compile (note the homebrew include/lib paths + linking fmt & spdlog):
//   clang++ -std=c++20 -O2 -Wall -Wextra -pthread \
//     -I/opt/homebrew/include -L/opt/homebrew/lib \
//     t5_libs.cpp -o t5libs -lspdlog -lfmt && ./t5libs
//
// What we measure: throughput of "log one already-formatted line", each tool doing
// its natural thing, sink = /dev/null. This is the app-visible cost per LOG call.
//   - ours & spdlog-async: enqueue to a ring, a background thread drains  (ASYNC)
//   - printf / iostream / fmt: format+write inline on the calling thread    (SYNC)
// We label async vs sync honestly; the resume compares against all four.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <array>
#include <atomic>
#include <thread>
#include <chrono>
#include <vector>
#include <string>
#include <fstream>

#include <fmt/core.h>
#include <fmt/os.h>
#include <spdlog/spdlog.h>
#include <spdlog/async.h>
#include <spdlog/sinks/basic_file_sink.h>

static constexpr std::size_t CACHE_LINE  = 128;
static constexpr std::size_t CAPACITY    = 8192;
static constexpr std::size_t MAX_MSG_LEN = 64;

struct Slot { char data[MAX_MSG_LEN]; uint16_t len = 0; };

// Our aligned lock-free SPSC ring (from T3+T5), consumer drains to a real file.
class SpscLogger {
public:
    explicit SpscLogger(const char* path) : out_(std::fopen(path, "w")) {}
    void start() { consumer_ = std::thread([this]{ consume_loop(); }); }
    bool push(const char* msg, std::size_t len) {
        std::uint64_t h = head_.load(std::memory_order_relaxed);
        std::uint64_t t = tail_.load(std::memory_order_acquire);
        if (h - t == CAPACITY) return false;
        std::size_t idx = h & (CAPACITY - 1);
        std::memcpy(buf_[idx].data, msg, len);
        buf_[idx].len = static_cast<uint16_t>(len);
        head_.store(h + 1, std::memory_order_release);
        return true;
    }
    void stop() { done_.store(true, std::memory_order_relaxed); consumer_.join();
                  if (out_) std::fclose(out_); }
private:
    void consume_loop() {
        std::uint64_t t = tail_.load(std::memory_order_relaxed);
        for (;;) {
            std::uint64_t h = head_.load(std::memory_order_acquire);
            if (t == h) {
                if (done_.load(std::memory_order_relaxed)) { tail_.store(t, std::memory_order_release); return; }
                continue;
            }
            while (t != h) {
                std::size_t idx = t & (CAPACITY - 1);
                std::fwrite(buf_[idx].data, 1, buf_[idx].len, out_);
                std::fputc('\n', out_);
                ++t;
            }
            tail_.store(t, std::memory_order_release);
        }
    }
    alignas(CACHE_LINE) std::atomic<std::uint64_t> head_{0};
    alignas(CACHE_LINE) std::atomic<std::uint64_t> tail_{0};
    std::array<Slot, CAPACITY> buf_{};
    std::atomic<bool> done_{false};
    std::FILE* out_ = nullptr;
    std::thread consumer_;
};

template <class F>
static double timed_rate(int n, F&& per_msg) {
    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < n; ++i) per_msg(i);
    auto t1 = std::chrono::steady_clock::now();
    return n / std::chrono::duration<double>(t1 - t0).count() / 1e6;   // M/s
}

int main() {
    const int N = 5'000'000;

    // Pre-format identical messages so every tool logs the same bytes.
    std::vector<std::string> msgs;
    msgs.reserve(N);
    for (int i = 0; i < N; ++i) {
        char m[48];
        int k = std::snprintf(m, sizeof m, "log line %d value=%d", i, i * 7);
        msgs.emplace_back(m, (std::size_t)k);
    }

    std::printf("=== Tier 5 library comparison (%d msgs, M msgs/sec, sink=/dev/null-ish) ===\n\n", N);

    // 1) OURS (async lock-free ring -> file)
    double ours;
    {
        SpscLogger log("/dev/null");
        log.start();
        ours = timed_rate(N, [&](int i){ while (!log.push(msgs[i].data(), msgs[i].size())) {} });
        log.stop();
    }

    // 2) printf (sync)
    double pf;
    {
        FILE* dev = std::fopen("/dev/null", "w");
        pf = timed_rate(N, [&](int i){ std::fprintf(dev, "%s\n", msgs[i].c_str()); });
        std::fclose(dev);
    }

    // 3) iostream (sync)
    double ios;
    {
        std::ofstream dev("/dev/null");
        ios = timed_rate(N, [&](int i){ dev << msgs[i] << '\n'; });
    }

    // 4) fmt (sync)
    double fm;
    {
        auto dev = fmt::output_file("/dev/null");
        fm = timed_rate(N, [&](int i){ dev.print("{}\n", msgs[i]); });
    }

    // 5) spdlog async (async, ring-buffered like ours)
    double sp;
    {
        spdlog::init_thread_pool(CAPACITY, 1);
        auto logger = spdlog::create_async<spdlog::sinks::basic_file_sink_mt>("bench", "/dev/null", true);
        logger->set_pattern("%v");                 // no timestamp formatting, just the message
        sp = timed_rate(N, [&](int i){ logger->info(msgs[i]); });
        logger->flush();
        spdlog::drop_all();
    }

    std::printf("ours  (async lock-free ring): %6.1f M/s\n", ours);
    std::printf("spdlog(async)              : %6.1f M/s\n", sp);
    std::printf("fmt   (sync)               : %6.1f M/s\n", fm);
    std::printf("printf(sync)               : %6.1f M/s\n", pf);
    std::printf("iostream (sync)            : %6.1f M/s\n", ios);
    std::printf("\nours vs spdlog(async): %.2fx | ours vs fmt: %.1fx | ours vs printf: %.1fx | ours vs iostream: %.1fx\n",
                ours / sp, ours / fm, ours / pf, ours / ios);
    return 0;
}
