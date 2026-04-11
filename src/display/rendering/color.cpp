#include "rendering/color.hpp"

glm::vec4 Color::GetFace(FaceFlawKind flaw)
{
    switch (flaw)
    {
    case FaceFlawKind::OVERHANG:
        return glm::vec4(FACE + STEP, FACE, FACE, 0.5f);
    case FaceFlawKind::SMALL_FEATURE:
    case FaceFlawKind::THIN_SECTION:
        return glm::vec4(FACE + STEP, FACE + STEP, FACE, 0.5f);
    default:
        return glm::vec4(0.0f);
    }
}

glm::vec4 Color::GetEdge(EdgeFlawKind flaw)
{
    switch (flaw)
    {
    case EdgeFlawKind::SHARP_CORNER:
        return glm::vec4(EDGE + STEP, EDGE, EDGE, 1.0f);
    default:
        return glm::vec4(EDGE, EDGE, EDGE, 1.0f);
    }
}