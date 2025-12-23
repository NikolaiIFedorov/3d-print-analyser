#include "clock.hpp"

void Clock::Start()
{
    startPoint = clockUtil.now();
}

float Clock::End()
{
    std::chrono::time_point endPoint = clockUtil.now();
    auto clockDuration = std::chrono::duration_cast<std::chrono::milliseconds>(endPoint - startPoint);

    float duration = clockDuration.count();
    return duration;
}
