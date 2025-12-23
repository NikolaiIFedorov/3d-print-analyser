#include "color.hpp"

glm::vec3 Color::GetFace(uint32_t id, const Scene &scene)
{
    Flaw flaw = Analysis::GetFlaw(id, scene);
    if (flaw == Flaw::OVERHANG)
    {
        return glm::vec3(FACE + FORM_STEP, FACE, FACE);
    }
    return glm::vec3(FACE, FACE, FACE);
}