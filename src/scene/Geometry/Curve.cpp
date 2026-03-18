#include "Curve.hpp"

#include "utils/log.hpp"

glm::dvec3 ArcCurve::Evaluate(double t, const glm::dvec3 &edgeStart, const glm::dvec3 &edgeEnd) const
{
    glm::dvec3 toStart = edgeStart - arc.center;
    glm::dvec3 toEnd = edgeEnd - arc.center;

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

glm::dvec3 NurbsCurve::Evaluate(double t, const glm::dvec3 &edgeStart, const glm::dvec3 &edgeEnd) const
{
    glm::dvec3 point = tinynurbs::curvePoint(*nurbs, t);
    return point;
}