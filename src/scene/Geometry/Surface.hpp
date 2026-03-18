#pragma once

#include <memory>
#include <glm/glm.hpp>
#include <tinynurbs/tinynurbs.h>
#include "Geometry.hpp"

struct Surface
{
    virtual ~Surface() = default;
    virtual glm::dvec3 GetNormal() const = 0;
    virtual bool IsPlanar() const { return false; }
};

struct PlanarSurface : Surface
{
    PlanarData data;

    PlanarSurface() : data{glm::dvec3(0, 0, 1), 0.0} {}
    PlanarSurface(const PlanarData &planarData) : data(planarData) {}

    glm::dvec3 GetNormal() const override { return data.normal; }
    bool IsPlanar() const override { return true; }

    const PlanarData &GetPlanarData() const { return data; }
    PlanarData &GetPlanarData() { return data; }
};

struct NurbsSurface : Surface
{
    std::unique_ptr<tinynurbs::RationalSurface3d> nurbs;

    NurbsSurface(std::unique_ptr<tinynurbs::RationalSurface3d> nurbsData) : nurbs(std::move(nurbsData)) {}

    glm::dvec3 GetNormal() const override
    {
        // NURBS face normal not yet fully implemented
        return glm::dvec3(0, 0, 1);
    }

    const tinynurbs::RationalSurface3d &GetNurbsData() const { return *nurbs; }
};
