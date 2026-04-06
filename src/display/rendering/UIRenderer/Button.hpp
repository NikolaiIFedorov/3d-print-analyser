#pragma once

#include <functional>
#include <string>

struct Button
{
    std::string panelId;
    std::function<void()> onClick;
};
