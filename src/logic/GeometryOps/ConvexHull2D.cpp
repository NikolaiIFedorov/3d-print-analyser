#include "ConvexHull2D.hpp"
#include "RingUtils.hpp"

#include <algorithm>
#include <limits>

namespace
{

struct IndexedPoint2D
{
    glm::dvec2 p;
    size_t idx;
};

double Cross(const glm::dvec2 &O, const glm::dvec2 &A, const glm::dvec2 &B)
{
    return (A.x - O.x) * (B.y - O.y) - (A.y - O.y) * (B.x - O.x);
}

} // namespace

namespace GeometryOps
{

std::vector<glm::dvec3> ConvexHull2D(const std::vector<glm::dvec3> &points, const glm::dvec3 &normalHint)
{
    if (points.size() < 3)
        return points;

    glm::dvec3 u, v;
    BuildPlanarBasis(normalHint, u, v);

    std::vector<IndexedPoint2D> pts;
    pts.reserve(points.size());
    for (size_t i = 0; i < points.size(); i++)
        pts.push_back({glm::dvec2(glm::dot(points[i], u), glm::dot(points[i], v)), i});

    std::sort(pts.begin(), pts.end(), [](const IndexedPoint2D &a, const IndexedPoint2D &b)
              { return (a.p.x < b.p.x) || (a.p.x == b.p.x && a.p.y < b.p.y); });

    if (pts.size() < 3)
    {
        std::vector<glm::dvec3> hull;
        hull.reserve(pts.size());
        for (const auto &pt : pts)
            hull.push_back(points[pt.idx]);
        return hull;
    }

    // Andrew's monotone chain: split points into an upper and lower boundary relative to the
    // line through the two extreme (leftmost/rightmost) points, each maintained as a convex
    // stack, then stitched together.
    const IndexedPoint2D p1 = pts.front();
    const IndexedPoint2D p2 = pts.back();

    std::vector<IndexedPoint2D> up, down;
    up.push_back(p1);
    down.push_back(p1);

    for (size_t i = 1; i + 1 < pts.size(); i++)
    {
        const double side = Cross(p1.p, pts[i].p, p2.p);
        if (side < 0.0)
        {
            while (up.size() >= 2 && Cross(up[up.size() - 2].p, up.back().p, pts[i].p) < 0.0)
                up.pop_back();
            up.push_back(pts[i]);
        }
        else if (side > 0.0)
        {
            while (down.size() >= 2 && Cross(down[down.size() - 2].p, down.back().p, pts[i].p) > 0.0)
                down.pop_back();
            down.push_back(pts[i]);
        }
        // side == 0.0: collinear with p1-p2 — contributes nothing to the hull boundary.
    }

    up.push_back(p2);
    down.push_back(p2);

    std::vector<glm::dvec3> hull;
    hull.reserve(up.size() + down.size());
    for (const auto &pt : up)
        hull.push_back(points[pt.idx]);
    // Continue CCW from p2 back to p1 along the lower boundary's interior points, in reverse.
    // p1 and p2 themselves are skipped — both already added via `up`.
    for (int k = static_cast<int>(down.size()) - 2; k >= 1; k--)
        hull.push_back(points[down[static_cast<size_t>(k)].idx]);

    return hull;
}

double MinWidth2D(const std::vector<glm::dvec3> &points, const glm::dvec3 &normalHint)
{
    const std::vector<glm::dvec3> hull = ConvexHull2D(points, normalHint);
    if (hull.size() < 3)
        return 0.0; // degenerate (collinear or fewer than 3 distinct directions) cross-section

    glm::dvec3 u, v;
    BuildPlanarBasis(normalHint, u, v);

    std::vector<glm::dvec2> hull2D(hull.size());
    for (size_t i = 0; i < hull.size(); i++)
        hull2D[i] = glm::dvec2(glm::dot(hull[i], u), glm::dot(hull[i], v));

    double minWidth = std::numeric_limits<double>::max();
    const size_t m = hull2D.size();
    for (size_t i = 0; i < m; i++)
    {
        const glm::dvec2 edge = hull2D[(i + 1) % m] - hull2D[i];
        const double edgeLen = glm::length(edge);
        if (edgeLen < 1e-12)
            continue;
        const glm::dvec2 perp(-edge.y / edgeLen, edge.x / edgeLen);

        double minProj = std::numeric_limits<double>::max();
        double maxProj = std::numeric_limits<double>::lowest();
        for (const auto &p : hull2D)
        {
            const double proj = glm::dot(p, perp);
            minProj = std::min(minProj, proj);
            maxProj = std::max(maxProj, proj);
        }
        minWidth = std::min(minWidth, maxProj - minProj);
    }

    return minWidth == std::numeric_limits<double>::max() ? 0.0 : minWidth;
}

} // namespace GeometryOps
