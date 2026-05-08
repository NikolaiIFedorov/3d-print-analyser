#pragma once

#include <functional>
#include <string>
#include <utility>

struct ImportProgress
{
    std::string phase;
    float progress01 = -1.0f;
};

using ImportProgressCallback = std::function<void(const ImportProgress &)>;

inline void ReportImportProgress(const ImportProgressCallback *callback, std::string phase, float progress01 = -1.0f)
{
    if (callback == nullptr || !*callback)
        return;

    (*callback)(ImportProgress{std::move(phase), progress01});
}

inline float MapImportProgress(float localProgress01, float rangeStart01, float rangeEnd01)
{
    if (localProgress01 < 0.0f)
        return -1.0f;
    if (localProgress01 > 1.0f)
        localProgress01 = 1.0f;
    return rangeStart01 + (rangeEnd01 - rangeStart01) * localProgress01;
}
