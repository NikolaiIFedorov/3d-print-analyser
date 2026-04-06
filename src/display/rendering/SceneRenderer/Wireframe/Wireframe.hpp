#pragma once

#include "Geometry/AllGeometry.hpp"

#include "rendering/color.hpp"
#include "scene/scene.hpp"

class Wireframe
{
public:
    void Generate(Scene *scene, std::vector<Vertex> &vertices, std::vector<uint32_t> &indices, const AnalysisResults *results) const;

private:
    void AddPoint(const Point *point,
                  std::vector<Vertex> &vertices,
                  std::vector<uint32_t> &indices, bool isEdge) const;

    void AddEdge(const Edge *edge,
                 std::vector<Vertex> &vertices,
                 std::vector<uint32_t> &indices, bool isFace) const;

    void AddLineEdge(const Edge *edge,
                     std::vector<Vertex> &vertices,
                     std::vector<uint32_t> &indices) const;

    void AddCurvedEdge(const Edge *edge,
                       std::vector<Vertex> &vertices,
                       std::vector<uint32_t> &indices) const;

    void AddFace(const Face *face,
                 std::vector<Vertex> &vertices,
                 std::vector<uint32_t> &indices, bool isSolid) const;
    void AddSolid(const Solid *solid,
                  std::vector<Vertex> &vertices,
                  std::vector<uint32_t> &indices, const AnalysisResults *results) const;

    void TessellateCurve(const Curve *curve,
                         const glm::dvec3 &start,
                         const glm::dvec3 &end,
                         std::vector<Vertex> &vertices,
                         std::vector<uint32_t> &indices) const;
};