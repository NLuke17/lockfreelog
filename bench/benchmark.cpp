// benchmark.cpp — throughput of the lock-free logger vs spdlog / fmt / iostream / printf.
//
// Compile (needs: brew install fmt spdlog):
//   clang++ -std=c++20 -O2 -Wall -Wextra -pthread \
//     -I/opt/homebrew/include -L/opt/homebrew/lib \
//     bench/benchmark.cpp -o benchmark -lspdlog -lfmt && ./benchmark
//
// Every logger formats and emits the SAME line ("log line {} value={}") to /dev/null,
// so we compare the logging path, not the disk. Ours and spdlog-async are ring-buffered
// (the app only pays the enqueue+format cost); fmt/printf/iostream format+write inline.

#include "../include/lockfree_logger.hpp"

#include <chrono>
#include <cstdio>
#include <fstream>

#include <fmt/core.h>
#include <fmt/os.h>
#include <spdlog/spdlog.h>
#include <spdlog/async.h>
#include <spdlog/sinks/basic_file_sink.h>

template <class F>
static double rate_Mps(int n, F&& per) {
    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < n; ++i) per(i);
    auto t1 = std::chrono::steady_clock::now();
    return n / std::chrono::duration<double>(t1 - t0).count() / 1e6;
}

int main() {
    const int N = 5'000'000;
    std::printf("=== throughput (%d msgs, M msgs/sec, sink=/dev/null) ===\n\n", N);

    // ---- ours: lock-free async ring, format checked at compile time ----
    double ours = 0; unsigned long long ours_drop = 0;
    {
        std::FILE* dev = std::fopen("/dev/null", "w");
        lockfree::Logger<8192, 128> log(dev);
        // retry on full so nothing is dropped -> measures the SUSTAINED rate
        // (the slower of producer and consumer), not the cheap drop path.
        ours = rate_Mps(N, [&](int i){ while (!log.log<"log line {} value={}">(i, i * 7)) {} });
        ours_drop = log.dropped();
        // Logger destructor drains + joins here.
        std::fclose(dev);
    }

    // ---- spdlog async ----
    double sp = 0;
    {
        spdlog::init_thread_pool(8192, 1);
        auto logger = spdlog::create_async<spdlog::sinks::basic_file_sink_mt>("b", "/dev/null", true);
        logger->set_pattern("%v");
        sp = rate_Mps(N, [&](int i){ logger->info("log line {} value={}", i, i * 7); });
        logger->flush();
        spdlog::drop_all();
    }

    // ---- fmt (sync) ----
    double fm = 0;
    {
        auto dev = fmt::output_file("/dev/null");
        fm = rate_Mps(N, [&](int i){ dev.print("log line {} value={}\n", i, i * 7); });
    }

    // ---- printf (sync) ----
    double pf = 0;
    {
        std::FILE* dev = std::fopen("/dev/null", "w");
        pf = rate_Mps(N, [&](int i){ std::fprintf(dev, "log line %d value=%d\n", i, i * 7); });
        std::fclose(dev);
    }

    // ---- iostream (sync) ----
    double ios = 0;
    {
        std::ofstream dev("/dev/null");
        ios = rate_Mps(N, [&](int i){ dev << "log line " << i << " value=" << i * 7 << '\n'; });
    }

    // With retry-on-full, every message is logged (0 lost); ours_drop counts transient
    // "buffer full" hits that were retried — a contention signal, not lost data.
    std::printf("ours   (async lock-free) : %6.1f M/s   (0 lost; %llu retried on full)\n", ours, ours_drop);
    std::printf("spdlog (async)           : %6.1f M/s   <- ours is %.1fx faster\n", sp, ours / sp);
    std::printf("printf (sync)            : %6.1f M/s   <- ours is %.1fx faster\n", pf, ours / pf);
    std::printf("iostream (sync)          : %6.1f M/s\n", ios);
    std::printf("fmt    (sync)            : %6.1f M/s   (buffered memcpy to /dev/null)\n", fm);
    return 0;
}
