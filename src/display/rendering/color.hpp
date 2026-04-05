#pragma once

#include <cstdint>
#include "glm/glm.hpp"
#include "logic/Analysis/Analysis.hpp"
#include "scene/scene.hpp"

static const float BASE = 0.1;
static const float FORM_STEP = 0.1f;

static const float FACE = BASE + FORM_STEP;
static const float EDGE = FACE + FORM_STEP;
static const float POINT = EDGE + FORM_STEP;

struct Color
{
    static glm::vec3 GetBase() { return glm::vec3(BASE, BASE, BASE); }
    static glm::vec4 GetUI(int depth, float alpha = 0.9f)
    {
        float v = BASE + FORM_STEP * depth;
        return glm::vec4(v, v, v, alpha);
    }
    static glm::vec3 GetEdge() { return glm::vec3(EDGE, EDGE, EDGE); }
    static glm::vec3 GetEdge(Flaw flaw);
    static glm::vec3 GetPoint() { return glm::vec3(POINT, POINT, POINT); }
    static glm::vec3 GetFace() { return glm::vec3(FACE, FACE, FACE); }
    static glm::vec3 GetFace(const Face *face);
};
