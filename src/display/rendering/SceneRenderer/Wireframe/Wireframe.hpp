#pragma once

#include "Geometry/AllGeometry.hpp"

#include "RenderBuffer/RenderBuffer.hpp"
#include "color.hpp"
#include "Analysis/Analysis.hpp"

class Wireframe
{
public:
    void Generate(const RenderBuffer &buffer, std::vector<Vertex> &vertices, std::vector<uint32_t> &indices) const;

private:
    void AddPoint(const Point *point,
                  std::vector<Vertex> &vertices,
                  std::vector<uint32_t> &indices) const;

    void AddEdge(const Edge *edge,
                 std::vector<Vertex> &vertices,
                 std::vector<uint32_t> &indices) const;

    void AddLineEdge(const Edge *edge,
                     std::vector<Vertex> &vertices,
                     std::vector<uint32_t> &indices) const;

    void AddCurvedEdge(const Edge *edge,
                       std::vector<Vertex> &vertices,
                       std::vector<uint32_t> &indices) const;

    void AddLayers(const std::vector<Layer> &layers,
                   std::vector<Vertex> &vertices,
                   std::vector<uint32_t> &indices) const;

    void AddLayer(const Layer &layer,
                  std::vector<Vertex> &vertices,
                  std::vector<uint32_t> &indices) const;

    void AddFace(const Face *face,
                 std::vector<Vertex> &vertices,
                 std::vector<uint32_t> &indices) const;
    void AddSolid(const Solid *solid,
                  std::vector<Vertex> &vertices,
                  std::vector<uint32_t> &indices) const;

    void TessellateCurve(const Curve *curve,
                         const glm::dvec3 &start,
                         const glm::dvec3 &end,
                         std::vector<Vertex> &vertices,
                         std::vector<uint32_t> &indices) const;
};