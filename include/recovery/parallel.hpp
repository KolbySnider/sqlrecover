#pragma once
/// @file
/// @brief Shared worker-pool pattern used by carving, file-carving, and
/// recovery: run fn(i) for i in [0, count), workers pulling indices off
/// a shared atomic counter. jthreads auto-join on scope exit.

#include <thread>
#include <vector>
#include <atomic>
#include <algorithm>
#include <cstddef>

namespace sqlrecover {

/// @brief Run fn(i) for each i in [0, count), across up to `workers`
/// threads. Safe to call with workers > count or count == 0.
template <typename F>
void parallel_for(size_t count, unsigned workers, F&& fn) {
    if (count == 0) return;
    unsigned n = static_cast<unsigned>(std::min<size_t>(std::max(1u, workers), count));
    std::atomic<size_t> next{0};

    std::vector<std::jthread> pool;
    pool.reserve(n);
    for (unsigned w = 0; w < n; ++w) {
        pool.emplace_back([&next, count, &fn] {
            for (;;) {
                size_t i = next.fetch_add(1);
                if (i >= count) return;
                fn(i);
            }
        });
    }
} // pool destructor joins every jthread

} // namespace sqlrecover
