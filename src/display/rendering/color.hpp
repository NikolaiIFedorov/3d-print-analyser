#pragma once

#include <cstdint>
#include "glm/glm.hpp"
#include "logic/Analysis/AnalysisTypes.hpp"

static const float BASE = 0.1;
static const float FORM_STEP = 0.1f;

static const float FACE = BASE + FORM_STEP;
static const float EDGE = FACE + FORM_STEP;
static const float POINT = EDGE + FORM_STEP;

static const glm::vec3 FACE_DEFAULT = glm::vec3(FACE, FACE, FACE);
static const glm::vec3 EDGE_DEFAULT = glm::vec3(EDGE, EDGE, EDGE);
static const glm::vec3 POINT_DEFAULT = glm::vec3(POINT, POINT, POINT);

struct Color
{
    static glm::vec3 GetBase() { return glm::vec3(BASE, BASE, BASE); }
    static glm::vec4 GetUI(int depth, float alpha = 1.0f)
    {
        float v = BASE + FORM_STEP * depth;
        return glm::vec4(v, v, v, alpha);
    }
    static glm::vec3 GetEdge()
    {
        return EDGE_DEFAULT;
    }
    static glm::vec3 GetPoint()
    {
        return POINT_DEFAULT;
    }
    static glm::vec3 GetFace()
    {
        return FACE_DEFAULT;
    }

    static glm::vec4 GetFaceOverlay(Flaw flaw);
    static glm::vec4 GetLayerOverlay(Flaw flaw);
};
