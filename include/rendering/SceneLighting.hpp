#pragma once

#include <glm/glm.hpp>

namespace SceneLighting
{
/// World-space direction toward the directional light (matches shaded patches in `basic.frag`).
inline glm::vec3 DirectionalLightDirWorld()
{
    return glm::normalize(glm::vec3(1.0f, 1.0f, 1.0f));
}

/// Diffuse brighten factor for scene mesh shading (`basic.frag` and lit wireframe).
inline float SceneMeshBrightenAmount()
{
    return 0.75f;
}
} // namespace SceneLighting
