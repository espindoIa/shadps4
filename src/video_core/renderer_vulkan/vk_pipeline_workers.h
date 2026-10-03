// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <future>
#include <mutex>
#include <thread>
#include <vector>

#include <fmt/format.h>

#include "common/thread.h"
#include "common/types.h"

namespace Vulkan {

/// Small thread pool that creates Vulkan pipelines off the GPU thread.
class PipelineWorkers {
public:
    explicit PipelineWorkers(u32 num_threads) {
        threads.reserve(num_threads);
        for (u32 i = 0; i < num_threads; ++i) {
            threads.emplace_back([this, i] { WorkerLoop(i); });
        }
    }

    ~PipelineWorkers() {
        {
            std::scoped_lock lk{mutex};
            stop = true;
        }
        cv.notify_all();
        for (auto& thread : threads) {
            thread.join();
        }
    }

    PipelineWorkers(const PipelineWorkers&) = delete;
    PipelineWorkers& operator=(const PipelineWorkers&) = delete;

    /// Queues a task. The returned future becomes ready once it has run.
    template <typename Func>
    std::future<void> Submit(Func&& func) {
        std::packaged_task<void()> task{std::forward<Func>(func)};
        auto future = task.get_future();
        {
            std::scoped_lock lk{mutex};
            tasks.push_back(std::move(task));
        }
        cv.notify_one();
        return future;
    }

    /// Leaves room for the GPU and present threads.
    static u32 DefaultThreadCount() {
        const u32 hw_threads = std::max(std::thread::hardware_concurrency(), 1U);
        return std::clamp(hw_threads / 4, 1U, 4U);
    }

private:
    void WorkerLoop(u32 index) {
        Common::SetCurrentThreadName(fmt::format("shadPS4:PipelineWorker{}", index).c_str());
        while (true) {
            std::packaged_task<void()> task;
            {
                std::unique_lock lk{mutex};
                cv.wait(lk, [this] { return stop || !tasks.empty(); });
                // Pending tasks are always drained, since pipelines wait for their build on
                // destruction.
                if (tasks.empty()) {
                    return;
                }
                task = std::move(tasks.front());
                tasks.pop_front();
            }
            task();
        }
    }

    std::mutex mutex;
    std::condition_variable cv;
    std::deque<std::packaged_task<void()>> tasks;
    std::vector<std::thread> threads;
    bool stop{};
};

} // namespace Vulkan
