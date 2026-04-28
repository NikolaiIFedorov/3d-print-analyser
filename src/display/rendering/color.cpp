#include "rendering/color.hpp"

glm::vec4 Color::GetFace(FaceFlawKind flaw)
{
    const float faceL = FaceL();
    const float step = Step();
    switch (flaw)
    {
    case FaceFlawKind::OVERHANG:
        return glm::vec4(faceL + step, faceL, faceL, 0.5f);
    case FaceFlawKind::SMALL_FEATURE:
        return glm::vec4(faceL + step, faceL + step, faceL, 0.5f);
    case FaceFlawKind::THIN_SECTION:
        return glm::vec4(faceL + step, faceL + step * 0.5f, faceL, 0.5f);
    case FaceFlawKind::STRINGING:
        return glm::vec4(faceL, faceL + step, faceL + step, 0.5f);
    default:
        return glm::vec4(0.0f);
    }
}

glm::vec4 Color::GetEdge(EdgeFlawKind flaw)
{
    const float edgeL = EdgeL();
    const float step = Step();
    switch (flaw)
    {
    case EdgeFlawKind::SHARP_CORNER:
        return glm::vec4(edgeL + step, edgeL, edgeL, 1.0f);
    default:
        return glm::vec4(edgeL, edgeL, edgeL, 1.0f);
    }
}