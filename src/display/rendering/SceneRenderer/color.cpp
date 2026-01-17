#include "color.hpp"

glm::vec3 Color::GetFace(const Face* face)
{
    Flaw flaw = Analysis::GetFlaw(face);
    if (flaw == Flaw::OVERHANG)
    {
        return glm::vec3(FACE + FORM_STEP, FACE, FACE);
    }
    return glm::vec3(FACE, FACE, FACE);
}