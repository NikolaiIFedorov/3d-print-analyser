#include "rendering/color.hpp"

glm::vec4 Color::GetFace(FaceFlawKind flaw)
{
    switch (flaw)
    {
    case FaceFlawKind::OVERHANG:
        return glm::vec4(Color::kFaceL + Color::kStep, Color::kFaceL, Color::kFaceL, 0.5f);
    case FaceFlawKind::SMALL_FEATURE:
        return glm::vec4(Color::kFaceL + Color::kStep, Color::kFaceL + Color::kStep, Color::kFaceL, 0.5f);
    case FaceFlawKind::THIN_SECTION:
        return glm::vec4(Color::kFaceL + Color::kStep, Color::kFaceL + Color::kStep * 0.5f, Color::kFaceL, 0.5f);
    case FaceFlawKind::STRINGING:
        return glm::vec4(Color::kFaceL, Color::kFaceL + Color::kStep, Color::kFaceL + Color::kStep, 0.5f);
    default:
        return glm::vec4(0.0f);
    }
}

glm::vec4 Color::GetEdge(EdgeFlawKind flaw)
{
    switch (flaw)
    {
    case EdgeFlawKind::SHARP_CORNER:
        return glm::vec4(Color::kEdgeL + Color::kStep, Color::kEdgeL, Color::kEdgeL, 1.0f);
    default:
        return glm::vec4(Color::kEdgeL, Color::kEdgeL, Color::kEdgeL, 1.0f);
    }
}