#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <future>
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

        std::optional<T> TryTake()
        {
            if (!future.valid())
                return std::nullopt;
            if (future.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready)
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
        std::future<T> future;
        std::shared_ptr<std::atomic<bool>> cancelRequested;
    };

    explicit TaskRunner(std::size_t workerCount = 1)
    {
        workers.reserve(workerCount);
        for (std::size_t i = 0; i < workerCount; ++i)
            workers.emplace_back([this]()
                                 { WorkerLoop(); });
    }

    ~TaskRunner()
    {
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
};
