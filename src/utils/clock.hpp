#pragma once

#include <chrono>

static std::chrono::steady_clock clockUtil;
static std::chrono::time_point startPoint = clockUtil.now();

class Clock
{
public:
    static void Start();
    static float End();

private:
};

#define CLOCK_START Clock::Start();
#define CLOCK_END Clock::End();