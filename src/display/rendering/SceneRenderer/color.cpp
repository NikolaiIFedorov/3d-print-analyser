#include "color.hpp"

glm::vec3 Color::GetFace(const Face *face)
{
    Flaw flaw = Analysis::Instance().FlawFace(face);
    switch (flaw)
    {
    case Flaw::OVERHANG:
        return glm::vec3(FACE + FORM_STEP, FACE, FACE);
    default:
        return glm::vec3(FACE, FACE, FACE);
    }
}

glm::vec3 Color::GetEdge(Flaw flaw)
{
    switch (flaw)
    {
    case Flaw::THIN_SECTION:
        return glm::vec3(EDGE + FORM_STEP, EDGE, EDGE);
    default:
        return glm::vec3(EDGE, EDGE, EDGE);
    }
}