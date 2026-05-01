#include "SystemAppearance.hpp"

namespace SystemAppearance
{
    bool IsDark()
    {
        return false;
    }

    void SetChangeCallback(std::function<void()>)
    {
    }

    void ClearChangeCallback()
    {
    }
}
