#pragma once

#include <unordered_set>
struct Edge;

#include <variant>
#include <tinynurbs/tinynurbs.h>
#include "Geometry.hpp"

#include <cmath>

struct Curve
{
    std::unordered_set<Edge *> dependencies;
    CurveType type;

    std::variant<ArcData, std::unique_ptr<tinynurbs::RationalCurve3d>> data;

    const ArcData &GetArc() const { return std::get<ArcData>(data); }
    bool IsArc() const { return std::holds_alternative<ArcData>(data); }

    const tinynurbs::RationalCurve3d &GetNurbs() const { return *std::get<std::unique_ptr<tinynurbs::RationalCurve3d>>(data); }
    bool IsNurbs() const { return std::holds_alternative<std::unique_ptr<tinynurbs::RationalCurve3d>>(data); }

    glm::dvec3 Evaluate(double t, const glm::dvec3 &edgeStart, const glm::dvec3 &edgeEnd) const;

private:
    glm::dvec3 EvaluateArc(double t, const glm::dvec3 &start, const glm::dvec3 &end) const;
    glm::dvec3 EvaluateNURBS(double t) const;
};
