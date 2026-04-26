#include "CalibPickSegments.hpp"

#include "scene.hpp"
#include "Geometry/Curve.hpp"
#include "Geometry/Face.hpp"
#include "Geometry/OrientedEdge.hpp"

namespace CalibPickSegments
{

namespace
{

constexpr int kCurveSegments = 16;

void AppendStraight(const Edge *e, const glm::dvec3 &a, const glm::dvec3 &b, std::vector<PickSegment> &out)
{
    PickSegment s;
    s.edge = e;
    s.v0 = a;
    s.v1 = b;
    out.push_back(s);
}

void AppendCurved(const Edge *e, const glm::dvec3 &p0, const glm::dvec3 &p1, Curve *curve,
                  std::vector<PickSegment> &out)
{
    for (int i = 0; i < kCurveSegments; ++i)
    {
        const double t0 = static_cast<double>(i) / kCurveSegments;
        const double t1 = static_cast<double>(i + 1) / kCurveSegments;
        const glm::dvec3 a = curve->Evaluate(t0, p0, p1);
        const glm::dvec3 b = curve->Evaluate(t1, p0, p1);
        AppendStraight(e, a, b, out);
    }
}

void AppendOrientedEdge(const OrientedEdge &oe, std::vector<PickSegment> &out)
{
    Edge *e = oe.edge;
    if (e == nullptr || e->startPoint == nullptr || e->endPoint == nullptr)
        return;
    const glm::dvec3 a = oe.GetStartPosition();
    const glm::dvec3 b = oe.GetEndPosition();
    if (e->curve == nullptr)
        AppendStraight(e, a, b, out);
    else
        AppendCurved(e, a, b, e->curve, out);
}

void AppendFaceBoundary(const Face *face, std::vector<PickSegment> &out)
{
    if (face == nullptr)
        return;
    for (const auto &loop : face->loops)
    {
        for (const auto &oe : loop)
            AppendOrientedEdge(oe, out);
    }
}

} // namespace

void Build(const Scene *scene, std::vector<PickSegment> &out)
{
    out.clear();
    if (scene == nullptr)
        return;

    for (const Solid &solid : scene->solids)
    {
        for (const Face *f : solid.faces)
            AppendFaceBoundary(f, out);
    }
    for (const Face &f : scene->faces)
    {
        if (f.dependency != nullptr)
            continue;
        AppendFaceBoundary(&f, out);
    }

    for (const Edge &edge : scene->edges)
    {
        if (edge.startPoint == nullptr || edge.endPoint == nullptr)
            continue;
        if (!edge.dependencies.empty())
            continue;
        const glm::dvec3 a(edge.startPoint->position);
        const glm::dvec3 b(edge.endPoint->position);
        if (edge.curve == nullptr)
            AppendStraight(&edge, a, b, out);
        else
            AppendCurved(&edge, a, b, edge.curve, out);
    }
}

} // namespace CalibPickSegments
