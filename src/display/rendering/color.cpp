#include "rendering/color.hpp"

glm::vec4 Color::GetFace(FaceFlawKind flaw)
{
    const float faceL = FaceL();
    const float step = Step();
    switch (flaw)
    {
    case FaceFlawKind::OVERHANG:
        return glm::vec4(faceL + step, faceL, faceL, 0.5f);
    case FaceFlawKind::NOT_ENOUGH_SPACE:
        return glm::vec4(faceL + step, faceL + step, faceL, 0.5f);
    case FaceFlawKind::INSTABILITY:
        return glm::vec4(faceL + step, faceL + step * 0.5f, faceL, 0.5f);
    case FaceFlawKind::LAYER_DIFFERENCE:
        return glm::vec4(faceL + step * 0.5f, faceL, faceL + step, 0.5f);
    case FaceFlawKind::STRINGING:
        return glm::vec4(faceL, faceL + step, faceL + step, 0.5f);
    case FaceFlawKind::SHARP_CORNER:
        return glm::vec4(faceL + step, faceL + step * 0.5f, faceL + step * 0.25f, 0.5f);
    default:
        return glm::vec4(0.0f);
    }
}