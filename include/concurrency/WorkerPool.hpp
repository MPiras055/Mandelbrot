#pragma once
#include <atomic>
#include <thread>
#include <vector>
#include "macro_util.hpp"

/**
 * @file WorkerPool.hpp
 * @brief Owns the render worker threads and their stop flag.
 *
 * @details Pure lifecycle container extracted from MandelbrotEngine. It does NOT
 * change the threading behaviour: threads are spawned via an explicit `start()`
 * (called from the engine constructor body, after every collaborator the worker
 * routine touches is fully constructed — spawning in this class's own
 * constructor would race that ordering), and the engine keeps driving the exact
 * `requestStop → wake parked workers → join` shutdown sequence.
 *
 * @note The worker routine itself (acquire/release, job dispatch) is unchanged and
 * still lives in MandelbrotEngine; this class only owns thread handles + the stop
 * flag.
 */
namespace concurrency {

class WorkerPool {
    std::vector<std::thread> workers_;
    CACHE_ALIGN std::atomic_bool stop_{false};
    CACHE_PAD(std::atomic_bool)

public:
    WorkerPool() = default;
    ~WorkerPool() { join(); }

    WorkerPool(const WorkerPool&) = delete;
    WorkerPool& operator=(const WorkerPool&) = delete;

    /// Default worker count: one fewer than the hardware concurrency, floored at 2.
    static unsigned int defaultSize() noexcept {
        unsigned int numCores = std::thread::hardware_concurrency();
        return (numCores > 2) ? (numCores - 1) : 2;
    }

    /**
     * @brief Spawns `count` threads, each invoking `routine(thread_id)`.
     * @param routine callable copied into every thread; invoked as routine(size_t).
     */
    template <typename Fn>
    void start(Fn routine, unsigned int count = defaultSize()) {
        workers_.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            workers_.emplace_back(routine, i);
        }
    }

    /// Checked by the worker routine to exit its loop.
    bool stopRequested() const noexcept {
        return stop_.load(std::memory_order_relaxed);
    }

    /// Raise the stop flag (the caller is responsible for waking parked workers).
    void requestStop() noexcept {
        stop_.store(true, std::memory_order_release);
    }

    size_t size() const noexcept { return workers_.size(); }

    /// Joins every worker; idempotent (safe to call from both the engine and dtor).
    void join() {
        for (std::thread& w : workers_) {
            if (w.joinable()) w.join();
        }
    }
};

} // namespace concurrency
