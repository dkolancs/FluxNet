#include "concurrency/mutex_queue.hpp"
#include "concurrency/spsc_ring.hpp"

#include <benchmark/benchmark.h>

static void spsc_ping(benchmark::State& state) {
    fluxnet::detail::SpscRing<int> ring(1024);
    for (auto _ : state) {
        if (!ring.try_push(1)) {
            int value = 0;
            ring.try_pop(value);
            ring.try_push(1);
        }
        int value = 0;
        ring.try_pop(value);
        benchmark::DoNotOptimize(value);
    }
}
BENCHMARK(spsc_ping);

static void mutex_ping(benchmark::State& state) {
    fluxnet::detail::MutexQueue<int> queue(1024);
    for (auto _ : state) {
        if (!queue.try_push(1)) {
            int value = 0;
            queue.try_pop(value);
            queue.try_push(1);
        }
        int value = 0;
        queue.try_pop(value);
        benchmark::DoNotOptimize(value);
    }
}
BENCHMARK(mutex_ping);

BENCHMARK_MAIN();
