#pragma once

#include <algorithm>
#include <cstdint>
#include "glm/glm.hpp"
#include "logic/Analysis/AnalysisTypes.hpp"

static const float BASE = 0.1;
static const float UI_BASE = BASE / 2.0f;
static const float STEP = 0.1f;
static const float GRID_EXTENT = 256.0f;

static const float FACE = BASE + STEP;
static const float EDGE = FACE + STEP;
static const float POINT = EDGE + STEP;

static const glm::vec3 FACE_DEFAULT = glm::vec3(FACE, FACE, FACE);
static const glm::vec3 EDGE_DEFAULT = glm::vec3(EDGE, EDGE, EDGE);
static const glm::vec3 POINT_DEFAULT = glm::vec3(POINT, POINT, POINT);

struct Color
{
    static glm::vec3 GetBase() { return glm::vec3(BASE, BASE, BASE); }
    static glm::vec4 GetUI(int depth, float alpha = 1.0f)
    {
        float v = UI_BASE + STEP * depth;
        return glm::vec4(v, v, v, alpha);
    }
    static glm::vec4 GetInputBg(int layer, float alpha = 1.0f)
    {
        return GetUI(std::max(layer, 2), alpha);
    }
    static glm::vec4 GetUIText(int depth, float alpha = 1.0f)
    {
        float v = UI_BASE + STEP * (depth + 5);
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

    static glm::vec3 GetGrid() { return glm::vec3(BASE + STEP, BASE + STEP, BASE + STEP); }
    static glm::vec3 GetAxisX(bool positive = true)
    {
        float v = positive ? BASE + 5 * STEP : BASE + 3 * STEP;
        return glm::vec3(v, BASE + STEP, BASE + STEP);
    }
    static glm::vec3 GetAxisY(bool positive = true)
    {
        float v = positive ? BASE + 5 * STEP : BASE + 3 * STEP;
        return glm::vec3(BASE + STEP, v, BASE + STEP);
    }
    static glm::vec3 GetAxisZ(bool positive = true)
    {
        float v = positive ? BASE + 5 * STEP : BASE + 3 * STEP;
        return glm::vec3(BASE + STEP, BASE + STEP, v);
    }

    static glm::vec4 GetFace(FaceFlawKind flaw);
    static glm::vec4 GetEdge(EdgeFlawKind flaw);
};
