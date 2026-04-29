#pragma once

#include <glm/mat4x4.hpp>

#include "RenderingExperiments.hpp"

/// Orthographic projection + default framebuffer depth convention for the viewport.
namespace ProjectionDepthMode
{
/// When `RenderingExperiments::kReverseZDepth` is true, returns `R * proj` with `R` flipping
/// clip-space Z so clearing depth to **0** and using **`GL_GEQUAL`** matches nearer fragments.
[[nodiscard]] inline glm::mat4 EffectiveProjection(const glm::mat4 &cameraOrthographicProj)
{
    if (!RenderingExperiments::kReverseZDepth)
        return cameraOrthographicProj;

    glm::mat4 flip(1.0f);
    flip[2][2] = -1.0f;
    return flip * cameraOrthographicProj;
}
} // namespace ProjectionDepthMode
