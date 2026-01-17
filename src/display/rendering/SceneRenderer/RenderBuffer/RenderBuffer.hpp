#pragma once

#include <vector>
#include <variant>
#include "Geometry/AllGeometry.hpp"

struct RenderBuffer
{
public:
    void Clear();
    void AddForm(FormPtr form);
    void RemoveForm(FormPtr form);

    const std::vector<FormPtr> &GetForms() const { return forms; }

private:
    std::vector<FormPtr> forms;
};