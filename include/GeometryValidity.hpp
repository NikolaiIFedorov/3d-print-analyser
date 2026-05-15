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

/// Weld nearby straight-edge endpoints (spatial hash + union), then remove faces whose
/// triangle / fan decomposition is still below the same scale-aware area threshold as
/// `EvaluateAppInvalidTagsForSolid`. Skips edges with `curve` or `bridgePoints` (no endpoint moves).
/// Clears removed faces' loops and `dependency` like coplanar merge defunct faces.
/// Returns whether any change was made. Does **not** refresh `Solid` validity cache — caller should.
struct DegenerateRepairStats
{
    std::size_t weldUnionPairs = 0;
    std::size_t edgesRetargeted = 0;
    std::size_t facesRemoved = 0;
};

[[nodiscard]] bool TryRepairDegenerateSolidBRep(Solid &solid, DegenerateRepairStats *statsOut = nullptr) noexcept;

/// For **clean** manifold edges (exactly two incident faces on `Edge::dependencies`): if both
/// faces use the same directed half-edge (A→B on both), flips winding on one **planar** incident
/// face and recomputes its plane. Iterates until stable or a cap. Skips non-planar faces and
/// edges that are not two-face manifold.
[[nodiscard]] bool TryRepairInconsistentFaceOrientationSolid(Solid &solid) noexcept;

/// Merge redundant **straight** `Edge` records that share the same undirected endpoint pair
/// (after weld, duplicate `Edge*` can remain). Retargets all solid face `OrientedEdge`s to the
/// canonical `Edge*` (smallest address), updates `Face` / `Point` dependency sets, and orphans
/// merged edges. Skips buckets containing curved or bridged edges.
[[nodiscard]] bool TryMergeDuplicateStraightEdgesSolid(Solid &solid) noexcept;

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

} // namespace GeometryValidity
