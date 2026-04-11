#include "Overhang.hpp"

#include <cmath>
#include <limits>
#include <numbers>

Overhang::Overhang(double maxAngleDeg)
    : minZComponent(-std::cos(maxAngleDeg * std::numbers::pi / 180.0))
{
}

static glm::dvec3 FaceCentroid(const Face *face)
{
    glm::dvec3 sum(0.0);
    int count = 0;
    for (const auto &loop : face->loops)
    {
        for (const auto &oe : loop)
        {
            sum += oe.GetStartPosition();
            count++;
        }
    }
    return sum / static_cast<double>(count);
}

// Check if a downward ray from point p intersects a planar face
static bool RayHitsFaceBelow(const glm::dvec3 &p, const Face *face)
{
    glm::dvec3 normal = face->GetSurface().GetNormal();

    // Face must not be vertical (normal must have a Z component)
    if (std::abs(normal.z) < 1e-9)
        return false;

    // Ray: p + t * (0, 0, -1). Plane: dot(normal, x) = d
    // Solve for t: normal.z * (-t) = d - dot(normal, p)
    double d = glm::dot(normal, face->loops[0][0].GetStartPosition());
    double t = (glm::dot(normal, p) - d) / normal.z;

    // Must be below (t > 0 means the hit is in the -Z direction)
    if (t < 1e-9)
        return false;

    glm::dvec3 hit = p - glm::dvec3(0.0, 0.0, t);

    // Project hit point onto the face's 2D plane and do point-in-polygon
    // Use the outer loop (first loop) for containment
    const auto &loop = face->loops[0];
    int crossings = 0;
    for (size_t i = 0; i < loop.size(); i++)
    {
        glm::dvec2 a(loop[i].GetStartPosition().x, loop[i].GetStartPosition().y);
        glm::dvec2 b(loop[i].GetEndPosition().x, loop[i].GetEndPosition().y);
        glm::dvec2 pt(hit.x, hit.y);

        if ((a.y <= pt.y && b.y > pt.y) || (b.y <= pt.y && a.y > pt.y))
        {
            double ti = (pt.y - a.y) / (b.y - a.y);
            if (pt.x < a.x + ti * (b.x - a.x))
                crossings++;
        }
    }
    return (crossings % 2) == 1;
}

static double SolidMinZ(const Solid *solid)
{
    double minZ = std::numeric_limits<double>::max();
    for (const Face *f : solid->faces)
        for (const auto &loop : f->loops)
            for (const auto &oe : loop)
                minZ = std::min(minZ, oe.GetStartPosition().z);
    return minZ;
}

static bool IsOnBuildPlate(const Face *face, double solidMinZ)
{
    for (const auto &loop : face->loops)
        for (const auto &oe : loop)
            if (oe.GetStartPosition().z > solidMinZ + 1e-6)
                return false;
    return true;
}

static bool IsSolidBelow(const Face *face)
{
    if (face->dependency == nullptr)
        return false;

    // Faces at the lowest Z of the solid rest on the build plate
    if (IsOnBuildPlate(face, SolidMinZ(face->dependency)))
        return true;

    glm::dvec3 centroid = FaceCentroid(face);

    // Test a point slightly below the face centroid
    glm::dvec3 testPoint = centroid - glm::dvec3(0.0, 0.0, 1e-6);

    // Count how many other faces a downward ray from testPoint intersects
    int crossings = 0;
    for (const Face *other : face->dependency->faces)
    {
        if (other == face)
            continue;
        if (RayHitsFaceBelow(testPoint, other))
            crossings++;
    }

    // Odd crossings = point is inside the solid = supported
    return (crossings % 2) == 1;
}

std::optional<FaceFlawKind> Overhang::Analyze(const Face *face) const
{
    glm::dvec3 normal = face->GetSurface().GetNormal();

    if (normal.z >= minZComponent)
        return std::nullopt;

    if (IsSolidBelow(face))
        return std::nullopt;

    return FaceFlawKind::OVERHANG;
}