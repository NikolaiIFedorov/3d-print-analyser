#include "RenderBuffer.hpp"
#include "utils/log.hpp"

void RenderBuffer::Clear()
{
    forms.clear();
}

void RenderBuffer::AddForm(FormPtr form)
{
    forms.push_back(form);
}

void RenderBuffer::RemoveForm(FormPtr form)
{
    auto it = std::find(forms.begin(), forms.end(), form);

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
