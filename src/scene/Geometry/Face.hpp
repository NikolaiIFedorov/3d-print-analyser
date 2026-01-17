#pragma once

#include <unordered_set>
struct Solid;

#include <vector>
struct Edge;

#include "Geometry.hpp"

struct Face
{
    std::unordered_set<Solid *> dependencies;

    std::vector<std::vector<Edge *>> loops;

    std::variant<PlanarData, std::unique_ptr<tinynurbs::RationalSurface3d>> surface;

    bool IsPlanar() const { return std::holds_alternative<PlanarData>(surface); }
    const PlanarData &GetPlanar() const { return std::get<PlanarData>(surface); }
    PlanarData &GetPlanar() { return std::get<PlanarData>(surface); }

    bool IsNurbs() const { return std::holds_alternative<std::unique_ptr<tinynurbs::RationalSurface3d>>(surface); }
    const tinynurbs::RationalSurface3d &GetNurbs() const { return *std::get<std::unique_ptr<tinynurbs::RationalSurface3d>>(surface); }

    Face() : surface(PlanarData{glm::dvec3(0, 0, 1), 0.0}) {}
};
