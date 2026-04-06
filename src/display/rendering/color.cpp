#include "rendering/color.hpp"

glm::vec3 Color::GetFace(Flaw flaw)
{
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
    case Flaw::SHARP_CORNER:
        return glm::vec3(EDGE, EDGE + FORM_STEP, EDGE);
    case Flaw::BRIDGING:
        return glm::vec3(EDGE, EDGE, EDGE + FORM_STEP);
    case Flaw::SMALL_FEATURE:
        return glm::vec3(EDGE + FORM_STEP, EDGE + FORM_STEP, EDGE);
    default:
        return glm::vec3(EDGE, EDGE, EDGE);
    }
}