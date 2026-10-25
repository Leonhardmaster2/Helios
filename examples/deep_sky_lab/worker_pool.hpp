// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
//
// Persistent worker threads for the deep-sky lab's simulations.  A simulation
// step issues several short parallel loops; starting threads for each one
// would cost more than the loops themselves.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace helios::examples {

// Puts the calling thread below normal priority, so that a simulation using
// every core never delays the thread that draws the frame.
void lowerCurrentThreadPriority() noexcept;

class WorkerPool {
  public:
    // `background` workers run below normal priority.
    explicit WorkerPool(std::uint32_t threads, bool background = false) {
        std::uint32_t const helpers = threads > 1U ? threads - 1U : 0U;
        workers_.reserve(helpers);
        for (std::uint32_t index = 0; index < helpers; ++index) {
            workers_.emplace_back([this, background] {
                if (background) {
                    lowerCurrentThreadPriority();
                }
                workerLoop();
            });
        }
    }

    ~WorkerPool() {
        {
            std::scoped_lock lock{mutex_};
            stopping_ = true;
            ++generation_;
        }
        wake_.notify_all();
        for (std::thread& worker : workers_) {
            worker.join();
        }
    }

    WorkerPool(WorkerPool const&) = delete;
    WorkerPool& operator=(WorkerPool const&) = delete;

    [[nodiscard]] std::uint32_t threadCount() const noexcept {
        return static_cast<std::uint32_t>(workers_.size()) + 1U;
    }

    // Calls body(index) once for every index in [0, count) and returns when
    // all calls have finished.  Indices are handed out one at a time, so the
    // bodies must be independent of each other; which thread runs an index
    // then cannot change the result.  The calling thread takes part.
    void run(std::size_t count, std::function<void(std::size_t)> const& body) {
        if (count == 0) {
            return;
        }
        if (workers_.empty() || count == 1) {
            for (std::size_t index = 0; index < count; ++index) {
                body(index);
            }
            return;
        }
        {
            std::scoped_lock lock{mutex_};
            body_ = &body;
            count_ = count;
            next_.store(0, std::memory_order_relaxed);
            busy_ = static_cast<std::uint32_t>(workers_.size());
            ++generation_;
        }
        wake_.notify_all();
        drain(body, count);
        std::unique_lock lock{mutex_};
        done_.wait(lock, [this] { return busy_ == 0; });
        body_ = nullptr;
    }

  private:
    void drain(std::function<void(std::size_t)> const& body, std::size_t count) {
        for (;;) {
            std::size_t const index = next_.fetch_add(1, std::memory_order_relaxed);
            if (index >= count) {
                break;
            }
            body(index);
        }
    }

    void workerLoop() {
        std::uint64_t seen = 0;
        for (;;) {
            std::function<void(std::size_t)> const* body = nullptr;
            std::size_t count = 0;
            {
                std::unique_lock lock{mutex_};
                wake_.wait(lock, [&] { return generation_ != seen; });
                seen = generation_;
                if (stopping_) {
                    return;
                }
                body = body_;
                count = count_;
            }
            drain(*body, count);
            {
                std::scoped_lock lock{mutex_};
                --busy_;
            }
            done_.notify_one();
        }
    }

    std::vector<std::thread> workers_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::condition_variable done_;
    std::function<void(std::size_t)> const* body_ = nullptr;
    std::size_t count_ = 0;
    std::atomic<std::size_t> next_{0};
    std::uint32_t busy_ = 0;
    std::uint64_t generation_ = 0;
    bool stopping_ = false;
};

} // namespace helios::examples
