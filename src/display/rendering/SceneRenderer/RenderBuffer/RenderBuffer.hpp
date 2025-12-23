#pragma once

#include <vector>
#include <cstdint>

struct RenderBuffer
{
public:
    void Clear();
    void AddForm(uint32_t id);
    void RemoveForm(uint32_t id);

    const std::vector<uint32_t> &GetForms() const { return forms; }

private:
    std::vector<uint32_t> forms;
};