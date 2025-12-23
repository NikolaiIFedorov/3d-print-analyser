#include "RenderBuffer.hpp"
#include "utils/log.hpp"

void RenderBuffer::Clear()
{
    forms.clear();
}

void RenderBuffer::AddForm(uint32_t id)
{
    forms.push_back(id);
}

void RenderBuffer::RemoveForm(uint32_t id)
{
    auto it = std::find(forms.begin(), forms.end(), id);

    if (it == forms.end())
    {
        return;
    }

    if (it != forms.end())
    {
        std::iter_swap(it, forms.end() - 1);
    }
    forms.pop_back();
}
