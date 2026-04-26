#pragma once

#include <mapbox/earcut.hpp>

#include "scene.hpp"

#include "rendering/color.hpp"
#include "rendering/ScenePick.hpp"

class Patch
{
public:
    void Generate(Scene *scene, std::vector<Vertex> &vertices, std::vector<uint32_t> &indices, int viewport[4],
                   const AnalysisResults *results, std::vector<PickTriangle> *pickOut = nullptr) const;

private:
    void AddFace(const Face *face,
                 std::vector<Vertex> &vertices,
                 std::vector<uint32_t> &indices, bool isSolid, const AnalysisResults *results,
                 std::vector<PickTriangle> *pickOut) const;

    void AddSolid(const Solid *solid,
                  std::vector<Vertex> &vertices,
                  std::vector<uint32_t> &indices, const AnalysisResults *results,
                  std::vector<PickTriangle> *pickOut) const;

    std::vector<glm::dvec3> TessellateCurveToPoints(
        const Curve *curve,
        const glm::dvec3 &start,
        const glm::dvec3 &end,
        int segments) const;
};