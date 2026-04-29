#pragma once

#include <chrono>
#include <functional>
#include <queue>
#include <string>
#include <utility>

// Runs staged apply-tasks on the main thread under a per-frame time budget.
// Pair with TaskRunner: background thread computes data, this pipeline applies it safely.
class MainThreadPipeline
{
public:
    using StepFn = std::function<bool(double remainingBudgetMs)>;

    struct Task
    {
        std::string name;
        StepFn step;
    };

    void Enqueue(std::string name, StepFn step)
    {
        tasks.push(Task{std::move(name), std::move(step)});
    }

    void Clear()
    {
        std::queue<Task> empty;
        tasks.swap(empty);
    }

    bool HasPending() const
    {
        return !tasks.empty();
    }

    // Returns true if at least one task step ran this frame.
    bool Process(double budgetMs)
    {
        using Clock = std::chrono::steady_clock;
        const Clock::time_point tStart = Clock::now();
        bool ranStep = false;

        while (!tasks.empty())
        {
            const double elapsedMs = std::chrono::duration<double, std::milli>(Clock::now() - tStart).count();
            double remainingMs = budgetMs - elapsedMs;
            if (remainingMs <= 0.0 && ranStep)
                break;
            if (remainingMs < 0.0)
                remainingMs = 0.0;

            Task &current = tasks.front();
            const bool done = current.step(remainingMs);
            ranStep = true;
            if (done)
                tasks.pop();
            else
                break;
        }

        return ranStep;
    }

private:
    std::queue<Task> tasks;
};
