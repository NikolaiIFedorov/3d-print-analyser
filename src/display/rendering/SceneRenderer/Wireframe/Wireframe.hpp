#pragma once

#include "scene/scene.hpp"
#include "Id.hpp"

#include "RenderBuffer/RenderBuffer.hpp"
#include "color.hpp"

class Wireframe
{
public:
    void Generate(const Scene &scene, const RenderBuffer &buffer, std::vector<Vertex> &vertices, std::vector<uint32_t> &indices, int viewport[4]) const;

private:
    void AddPoint(const Scene &scene, uint32_t id,
                  std::vector<Vertex> &vertices,
                  std::vector<uint32_t> &indices) const;
    void AddEdge(const Scene &scene, uint32_t id,
                 std::vector<Vertex> &vertices,
                 std::vector<uint32_t> &indices) const;

    void AddStraightEdge(const Scene &scene, const Edge *edge,
                         std::vector<Vertex> &vertices,
                         std::vector<uint32_t> &indices) const;

    void AddCurvedEdge(const Scene &scene, const Edge *edge,
                       std::vector<Vertex> &vertices,
                       std::vector<uint32_t> &indices) const;

    void AddFace(const Scene &scene, uint32_t id,
                 std::vector<Vertex> &vertices,
                 std::vector<uint32_t> &indices) const;
    void AddSolid(const Scene &scene, uint32_t id,
                  std::vector<Vertex> &vertices,
                  std::vector<uint32_t> &indices) const;

    void TessellateCurve(const Curve *curve,
                         const glm::dvec3 &start,
                         const glm::dvec3 &end,
                         std::vector<Vertex> &vertices,
                         std::vector<uint32_t> &indices) const;
};