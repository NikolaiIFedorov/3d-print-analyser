#include "Curve.hpp"

#include "utils/log.hpp"

glm::dvec3 Curve::Evaluate(double t, const glm::dvec3 &edgeStart, const glm::dvec3 &edgeEnd) const
{
    switch (type)
    {
    case CurveType::ARC:
        return EvaluateArc(t, edgeStart, edgeEnd);
        break;

    case CurveType::NURBS:
        return EvaluateNURBS(t);
        break;
    default:
        return glm::dvec3{0, 0, 0};
        break;
    }
}

glm::dvec3 Curve::EvaluateArc(double t, const glm::dvec3 &start, const glm::dvec3 &end) const
{
    const ArcData &arc = GetArc();

    glm::dvec3 toStart = start - arc.center;
    glm::dvec3 toEnd = end - arc.center;

    double startAngle = std::atan2(toStart.y, toStart.x);
    double endAngle = std::atan2(toEnd.y, toEnd.x);

    double angleDiff = endAngle - startAngle;
    if (angleDiff > M_PI)
        angleDiff -= 2 * M_PI;
    else if (angleDiff < -M_PI)
        angleDiff += 2 * M_PI;

    double angle = startAngle + t * angleDiff;

    glm::dvec3 point = arc.center + glm::dvec3(arc.radius * std::cos(angle), arc.radius * std::sin(angle), 0.0);
    return point;
}

glm::dvec3 Curve::EvaluateNURBS(double t) const
{
    const tinynurbs::RationalCurve3d &nurbs = GetNurbs();

    glm::dvec3 poimt = tinynurbs::curvePoint(nurbs, t);

    return poimt;
}