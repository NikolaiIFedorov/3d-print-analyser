#pragma once

#include <unordered_set>
struct Edge;

#include <memory>
#include <tinynurbs/tinynurbs.h>
#include "Geometry.hpp"

#include <cmath>

struct Curve
{
    std::unordered_set<Edge *> dependencies;

    virtual ~Curve() = default;
    virtual glm::dvec3 Evaluate(double t, const glm::dvec3 &edgeStart, const glm::dvec3 &edgeEnd) const = 0;
};

struct ArcCurve : Curve
{
    ArcData arc;

    ArcCurve(const ArcData &arcData) : arc(arcData) {}

    glm::dvec3 Evaluate(double t, const glm::dvec3 &edgeStart, const glm::dvec3 &edgeEnd) const override;
};

struct NurbsCurve : Curve
{
    std::unique_ptr<tinynurbs::RationalCurve3d> nurbs;

    NurbsCurve(std::unique_ptr<tinynurbs::RationalCurve3d> nurbsData) : nurbs(std::move(nurbsData)) {}

    glm::dvec3 Evaluate(double t, const glm::dvec3 &edgeStart, const glm::dvec3 &edgeEnd) const override;
};
