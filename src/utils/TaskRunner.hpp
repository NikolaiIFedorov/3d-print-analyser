#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

class TaskRunner
{
public:
    /// UI thread: max wall time spent waiting on one `TryTake(maxWait)` call, and the first bounded
    /// wait inside `TaskHandle::~TaskHandle` / `ReleaseFutureNonBlocking` before deferring to a
    /// detached waiter. Matches a single ~60fps frame slice.
    static constexpr std::chrono::milliseconds kUiAsyncFutureCompletionBudget{16};

    class CancellationToken
    {
    public:
        CancellationToken() = default;
        explicit CancellationToken(std::shared_ptr<std::atomic<bool>> cancelFlag)
            : cancelRequested(std::move(cancelFlag))
        {
        }

        bool IsCancellationRequested() const
        {
            return cancelRequested && cancelRequested->load(std::memory_order_relaxed);
        }

    private:
        std::shared_ptr<std::atomic<bool>> cancelRequested;
    };

    template <typename T>
    class TaskHandle
    {
    public:
        TaskHandle() = default;

        explicit TaskHandle(std::future<T> taskFuture, std::shared_ptr<std::atomic<bool>> cancelFlag)
            : future(std::move(taskFuture)),
              cancelRequested(std::move(cancelFlag))
        {
        }

        TaskHandle(const TaskHandle &) = delete;
        TaskHandle &operator=(const TaskHandle &) = delete;
        TaskHandle(TaskHandle &&) noexcept = default;
        TaskHandle &operator=(TaskHandle &&other)
        {
            if (this == &other)
                return *this;
            // Defaulted move-assignment would assign into `std::future`, whose destructor on the
            // replaced state can block — same failure mode as `~future` on a running task.
            ReleaseFutureNonBlocking();
            future = std::move(other.future);
            cancelRequested = std::move(other.cancelRequested);
            return *this;
        }

        ~TaskHandle()
        {
            ReleaseFutureNonBlocking();
        }

        /// Returns the result when ready within `maxWait` (zero = do not block; poll-only).
        std::optional<T> TryTake(std::chrono::milliseconds maxWait = std::chrono::milliseconds(0))
        {
            if (!future.valid())
                return std::nullopt;
            if (future.wait_for(maxWait) != std::future_status::ready)
                return std::nullopt;
            return future.get();
        }

        bool IsValid() const
        {
            return future.valid();
        }

        void RequestCancel() const
        {
            if (cancelRequested)
                cancelRequested->store(true, std::memory_order_relaxed);
        }

    private:
        void ReleaseFutureNonBlocking() noexcept
        {
            // `std::future::~future` waits if the task is still running — that blocked the UI thread
            // whenever a pending import/analysis/structure handle was reset while the worker was
            // inside a long CGAL call. Defer `get()` to a detached thread when not yet ready.
            if (!future.valid())
                return;
            if (future.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready)
            {
                try
                {
                    (void)future.get();
                }
                catch (...)
                {
                }
                return;
            }
            if (future.wait_for(TaskRunner::kUiAsyncFutureCompletionBudget) == std::future_status::ready)
            {
                try
                {
                    (void)future.get();
                }
                catch (...)
                {
                }
                return;
            }
            auto holder = std::shared_ptr<std::future<T>>(new std::future<T>(std::move(future)));
            std::thread([holder]() {
                try
                {
                    if (holder->valid())
                        (void)holder->get();
                }
                catch (...)
                {
                }
            }).detach();
        }

        std::future<T> future;
        std::shared_ptr<std::atomic<bool>> cancelRequested;
    };

    /// Single worker queue for import/analysis (and other jobs). Structure staging carve uses a
    /// separate `TaskRunner` in `display.cpp` (`StructureCarveTaskRunner`) so a stuck CGAL solid
    /// does not starve this queue.
    explicit TaskRunner(std::size_t workerCount = 1)
    {
        workers.reserve(workerCount);
        for (std::size_t i = 0; i < workerCount; ++i)
            workers.emplace_back([this]()
                                 { WorkerLoop(); });
    }

    /// Process-exit / emergency shutdown: stop accepting jobs, discard queued work, **detach** workers
    /// so the caller never blocks on `join()`. After this, **do not destroy** this `TaskRunner` — detached
    /// threads may still touch `queueMutex` until they observe `stopRequested` and exit `WorkerLoop`
    /// (same contract as the leaked Structure carve runner). Typical use: `unique_ptr::release()`.
    void RequestStopClearQueueAndDetachWorkers()
    {
        {
            std::lock_guard<std::mutex> lock(queueMutex);
            stopRequested = true;
            while (!jobQueue.empty())
                jobQueue.pop();
        }
        queueCv.notify_all();
        for (std::thread &worker : workers)
        {
            if (worker.joinable())
                worker.detach();
        }
        workers.clear();
        workersDetachedForExit = true;
    }

    ~TaskRunner()
    {
        if (workersDetachedForExit)
            return;
        {
            std::lock_guard<std::mutex> lock(queueMutex);
            stopRequested = true;
        }
        queueCv.notify_all();
        for (std::thread &worker : workers)
        {
            if (worker.joinable())
                worker.join();
        }
    }

    template <typename Fn>
    auto Submit(Fn &&fn)
        -> TaskHandle<std::invoke_result_t<Fn, const CancellationToken &>>
    {
        using T = std::invoke_result_t<Fn, const CancellationToken &>;

        auto cancelFlag = std::make_shared<std::atomic<bool>>(false);
        auto task = std::make_shared<std::packaged_task<T()>>(
            [callable = std::forward<Fn>(fn), token = CancellationToken(cancelFlag)]() mutable
            { return callable(token); });
        std::future<T> fut = task->get_future();

        {
            std::lock_guard<std::mutex> lock(queueMutex);
            jobQueue.emplace([task]()
                             { (*task)(); });
        }
        queueCv.notify_one();
        return TaskHandle<T>(std::move(fut), std::move(cancelFlag));
    }

private:
    void WorkerLoop()
    {
        while (true)
        {
            std::function<void()> job;
            {
                std::unique_lock<std::mutex> lock(queueMutex);
                queueCv.wait(lock, [this]()
                             { return stopRequested || !jobQueue.empty(); });
                if (stopRequested && jobQueue.empty())
                    return;
                job = std::move(jobQueue.front());
                jobQueue.pop();
            }
            job();
        }
    }

    std::mutex queueMutex;
    std::condition_variable queueCv;
    std::queue<std::function<void()>> jobQueue;
    std::vector<std::thread> workers;
    bool stopRequested = false;
    bool workersDetachedForExit = false;
};
