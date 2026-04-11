#pragma once

#include <functional>
#include <string>

struct Button
{
    std::string panelId;
    std::function<void()> onClick;
};

struct SectionButton
{
    std::string panelId;
    std::string sectionId;
    std::function<void()> onClick;
};

struct SectionSlider
{
    std::string panelId;
    std::string sectionId;
    double min = 0.0;
    double max = 1.0;
    double step = 0.1;
    double *value = nullptr;
    std::function<void()> onChange;
};
