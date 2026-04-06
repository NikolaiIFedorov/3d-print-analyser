#include "rendering/color.hpp"

glm::vec4 Color::GetFaceOverlay(Flaw flaw)
{
    switch (flaw)
    {
    case Flaw::OVERHANG:
        return glm::vec4(FACE + FORM_STEP, FACE, FACE, 0.35f);
    default:
        return glm::vec4(0.0f);
    }
}

glm::vec4 Color::GetLayerOverlay(Flaw flaw)
{
    switch (flaw)
    {
    case Flaw::THIN_SECTION:
        return glm::vec4(EDGE + FORM_STEP, EDGE, EDGE, 1.0f);
    case Flaw::SHARP_CORNER:
        return glm::vec4(EDGE, EDGE + FORM_STEP, EDGE, 1.0f);
    case Flaw::BRIDGING:
        return glm::vec4(EDGE, EDGE, EDGE + FORM_STEP, 1.0f);
    case Flaw::SMALL_FEATURE:
        return glm::vec4(EDGE + FORM_STEP, EDGE + FORM_STEP, EDGE, 1.0f);
    default:
        return glm::vec4(EDGE, EDGE, EDGE, 1.0f);
    }
}