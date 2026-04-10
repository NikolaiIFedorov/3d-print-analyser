#include "rendering/color.hpp"

glm::vec4 Color::GetFace(Flaw flaw)
{
    switch (flaw)
    {
    case Flaw::OVERHANG:
        return glm::vec4(FACE + STEP, FACE, FACE, 1.0f);
    case Flaw::SMALL_FEATURE:
    case Flaw::SHARP_CORNER:
        return glm::vec4(FACE + STEP, FACE + STEP, FACE, 1.0f);
    case Flaw::THIN_SECTION:
        return glm::vec4(FACE, FACE, FACE + STEP, 1.0f);
    default:
        return glm::vec4(0.0f);
    }
}

glm::vec4 Color::GetEdge(Flaw flaw)
{
    switch (flaw)
    {
    case Flaw::OVERHANG:
        return glm::vec4(EDGE + STEP, EDGE, EDGE, 1.0f);
    case Flaw::THIN_SECTION:
        return glm::vec4(EDGE, EDGE, EDGE + STEP, 1.0f);
        return glm::vec4(EDGE, EDGE + STEP, EDGE, 1.0f);
    case Flaw::SMALL_FEATURE:
    case Flaw::SHARP_CORNER:
        return glm::vec4(EDGE + STEP, EDGE + STEP, EDGE, 1.0f);
    default:
        return glm::vec4(EDGE, EDGE, EDGE, 1.0f);
    }
}