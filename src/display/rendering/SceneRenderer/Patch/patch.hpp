#pragma once

#include <mapbox/earcut.hpp>

#include "scene.hpp"

#include "RenderBuffer/RenderBuffer.hpp"
#include "color.hpp"

class Patch
{
public:
    void Generate(const RenderBuffer &buffer, std::vector<Vertex> &vertices, std::vector<uint32_t> &indices, int viewport[4]) const;

private:
    void AddFace(const Face *face,
                 std::vector<Vertex> &vertices,
                 std::vector<uint32_t> &indices) const;

    void AddSolid(const Solid *solid,
                  std::vector<Vertex> &vertices,
                  std::vector<uint32_t> &indices) const;

    std::vector<glm::dvec3> TessellateCurveToPoints(
        const Curve *curve,
        const glm::dvec3 &start,
        const glm::dvec3 &end,
        int segments) const;
};