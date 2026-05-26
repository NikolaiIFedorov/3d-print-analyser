#pragma once

#include <cstdint>
#include <string>
#include <type_traits>
#include <vector>

/// Canonical **valid / invalid** vocabulary for mesh and scene geometry.
///
/// **Scope**
/// - **App** flags: CAD_OpenGL’s own notion of bad topology/geometry, independent of CGAL.
/// - **CGAL** tags: outcomes that depend on CGAL Polygon Mesh Processing definitions
///   (`is_polygon_soup_a_polygon_mesh`, repair, empty mesh, etc.). Another library would need
///   its own parallel enum — definitions are not portable.
///
/// **Intentionally omitted for now**
/// - Structure-tool pick rules (planar-only, single loop, “upward enough”, min span) live in
///   `Display::IsStructureFaceEligible` until that pipeline stabilizes; do not duplicate them here
///   yet to avoid two sources of truth.

struct Solid;
struct Edge;
class Scene;

namespace GeometryValidity
{

enum class AppState : std::uint8_t
{
    Valid = 0,
    Invalid = 1,
};

/// App-layer invalidity (bitmask). Add values as detectors and UX copy are added.
enum class AppInvalidTag : std::uint32_t
{
    None = 0,
    DegenerateTriangle = 1u << 0,
    NullOrEmptyTopology = 1u << 1,
    SelfIntersection = 1u << 2,
    OpenBoundary = 1u << 3,
    NonManifoldConnectivity = 1u << 4,
    InconsistentFaceOrientation = 1u << 5,
};

constexpr AppInvalidTag operator|(AppInvalidTag a, AppInvalidTag b) noexcept
{
    using U = std::underlying_type_t<AppInvalidTag>;
    return static_cast<AppInvalidTag>(static_cast<U>(a) | static_cast<U>(b));
}

constexpr AppInvalidTag operator&(AppInvalidTag a, AppInvalidTag b) noexcept
{
    using U = std::underlying_type_t<AppInvalidTag>;
    return static_cast<AppInvalidTag>(static_cast<U>(a) & static_cast<U>(b));
}

constexpr AppInvalidTag &operator|=(AppInvalidTag &a, AppInvalidTag b) noexcept
{
    a = a | b;
    return a;
}

constexpr bool Any(AppInvalidTag flags) noexcept
{
    return flags != AppInvalidTag::None;
}

constexpr AppState AppStateFromTags(AppInvalidTag flags) noexcept
{
    return Any(flags) ? AppState::Invalid : AppState::Valid;
}

/// Scans `solid` faces/edges for issues the app treats as invalid independent of CGAL.
/// **`SelfIntersection` (v1):** proper segment–segment crossings in **planar face** boundary loops
/// projected to the face plane (each loop and between distinct loops). Non-planar faces are not
/// classified here yet (no flag from curved patches); triangle-soup tests can extend this later.
[[nodiscard]] AppInvalidTag EvaluateAppInvalidTagsForSolid(const Solid &solid) noexcept;

/// Collects **straight and curved** `Edge` pointers that participate in an open boundary, using the
/// same directed-use grouping as `AppInvalidTag::OpenBoundary` in `EvaluateAppInvalidTagsForSolid`.
/// Clears `out` first; order is not stable.
void CollectOpenBoundaryEdgesForSolid(const Solid &solid, std::vector<const Edge *> &out) noexcept;

    /// Human-readable list of set bits for logs / diagnostics (comma-separated; empty if `None`).
    [[nodiscard]] std::string DescribeAppInvalidTagsForLog(AppInvalidTag flags);

    void RefreshSolidAppGeometryValidityCache(Solid &solid) noexcept;
    void InvalidateSolidAppGeometryValidityCache(Solid &solid) noexcept;

    /// Uses OCCT ShapeFix_Shape to heal the solid's topology (orientations, degenerate edges, open boundaries).
    /// If changes are made, it repopulates the Solid's CAD_OpenGL structs via the scene.
    /// Returns whether any change was made. Does **not** refresh `Solid` validity cache — caller should.
    [[nodiscard]] bool TryRepairSolidBRep(Scene *scene, Solid &solid) noexcept;

    /// CGAL PMP polygon-soup / mesh lifecycle — map from concrete checks (`StructureCarve`, etc.).
enum class CgalPolygonSoupTag : std::uint8_t
{
    Unknown = 0,
    /// `PMP::is_polygon_soup_a_polygon_mesh` succeeded before conversion.
    SoupIsPolygonMesh,
    /// Initial soup failed the check; `repair_polygon_soup` produced a valid mesh.
    SoupInvalidRepairedOk,
    /// Still not a polygon mesh after repair (precondition would fail on `polygon_soup_to_polygon_mesh`).
    SoupInvalidStillInvalid,
    /// Conversion ran but the mesh has no faces (or equivalent empty result).
    MeshEmpty,
};

/// Debug counters for how edge-use grouping classified a solid's boundary/connectivity.
struct OpenBoundaryDebugStats
{
    std::size_t edgeGroupsOpenBoundary = 0;          // count == 1
    std::size_t edgeGroupsManifoldOpposite = 0;      // count == 2, opposite directions
    std::size_t edgeGroupsCount2SameDirection = 0;   // count == 2, same direction
    std::size_t edgeGroupsCount2NonOpposite = 0;     // count == 2, neither opposite nor same
    std::size_t edgeGroupsNonManifold3Plus = 0;      // count > 2
    std::size_t highlightedEdgeCount = 0;            // how many Edge* would be blamed as open
};

/// Uses the same straight-edge grouping + directed-use classification as app validity tags.
[[nodiscard]] OpenBoundaryDebugStats CollectOpenBoundaryDebugStatsForSolid(const Solid &solid) noexcept;

} // namespace GeometryValidity
