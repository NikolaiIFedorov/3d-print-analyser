#include <cstdint>
#include <vector>

#include "Geometry.hpp"

struct Face
{
    uint32_t id;

    std::vector<std::vector<uint32_t>> loops;

    std::variant<PlanarData, std::unique_ptr<tinynurbs::RationalSurface3d>> surface;

    mutable std::vector<Vertex> cachedVertices;
    mutable std::vector<uint32_t> cachedIndices;
    mutable bool tessellationDirty = true;

    glm::vec3 color = glm::vec3(0.2f, 0.2f, 0.2f);

    Face() : surface(PlanarData{glm::dvec3(0, 0, 1), 0.0}) {}

    bool IsPlanar() const { return std::holds_alternative<PlanarData>(surface); }
    bool IsNurbs() const { return std::holds_alternative<std::unique_ptr<tinynurbs::RationalSurface3d>>(surface); }

    PlanarData &GetPlanar() { return std::get<PlanarData>(surface); }
    const PlanarData &GetPlanar() const { return std::get<PlanarData>(surface); }

    tinynurbs::RationalSurface3d &GetNurbs() { return *std::get<std::unique_ptr<tinynurbs::RationalSurface3d>>(surface); }
    const tinynurbs::RationalSurface3d &GetNurbs() const { return *std::get<std::unique_ptr<tinynurbs::RationalSurface3d>>(surface); }

    std::vector<std::vector<uint32_t>> &GetLoops() { return loops; }

    const std::vector<uint32_t> &GetOuterLoop() const { return loops[0]; }
    std::vector<uint32_t> &GetOuterLoop() { return loops[0]; }

    bool HasHoles() const { return loops.size() > 1; }
    size_t GetHoleCount() const { return loops.size() > 1 ? loops.size() - 1 : 0; }
    const std::vector<uint32_t> &GetHole(size_t index) const { return loops[index + 1]; }
};
