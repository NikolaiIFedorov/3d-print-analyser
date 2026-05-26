#pragma once

#include <deque>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <cstddef>
#include "Geometry/AllGeometry.hpp"

/// Filled during `MergeCoplanarFaces` (and baseline topology via `CollectCoplanarMergeTopology`).
struct MergeCoplanarDiagnostics
{
    bool mergeRan = true;

    std::size_t facesBefore = 0;
    std::size_t facesAfter = 0;
    std::size_t mergeWhileIterations = 0;
    std::size_t mergeOperations = 0;
    std::size_t boundaryLoopFailures = 0;

    std::size_t edgesOneFaceBefore = 0;
    std::size_t edgesTwoFacesBefore = 0;
    std::size_t edgesThreePlusBefore = 0;
    std::size_t edgesOneFaceAfter = 0;
    std::size_t edgesTwoFacesAfter = 0;
    std::size_t edgesThreePlusAfter = 0;

    double bboxDiagonal = 0.0;
    double planeTolUsed = 0.0;
};

using SceneProgressCallback = std::function<void(float progress01)>;

class Scene
{
public:
    std::deque<Point> points;
    Point *CreatePoint(const glm::dvec3 &position);

    std::deque<Edge> edges;
    Edge *CreateEdge(Point *startPoint, Point *endPoint);
    Edge *CreateEdge(Point *startPoint, Point *endPoint, Curve *curve);
    Edge *CreateEdge(Point *startPoint, Point *endPoint, const std::vector<Point *> &bridgePoints);

    std::deque<std::unique_ptr<Curve>> curves;
    Curve *CreateCurve(glm::dvec3 centerPoint, double radius);
    Curve *CreateCurve(const tinynurbs::RationalCurve3d &nurbs);

    std::deque<Face> faces;
    Face *CreateFace(const std::vector<std::vector<Edge *>> &edgeLoops);
    Face *CreateFace(const std::vector<std::vector<Edge *>> &edgeLoops, std::unique_ptr<Surface> surf);

    std::deque<Solid> solids;
    /// When `runTopologyRepairs` is false, keeps imported topology unchanged and only refreshes validity tags.
    Solid *CreateSolid(const std::vector<Face *> &faces, bool runTopologyRepairs = true);

    std::deque<Compound> compounds;
    /// Non-owning references to `Solid`s in this scene. Nulls and unknown pointers skipped;
    /// duplicates removed (first occurrence kept). Returns nullptr if no members remain.
    Compound *CreateCompound(std::vector<Solid *> members);

    /// If there are at least two solids and no compounds yet, creates one compound listing every solid
    /// in `solids` iteration order. No-op otherwise (idempotent for single-solid scenes).
    void TryCreateCompoundWrappingAllSolidsIfNone() noexcept;

    /// Inserts `s` and every other solid in the same compound as `s` (if any) into `out`.
    void InsertSolidWithCompoundMembers(const Solid *s, std::unordered_set<const Solid *> &out) const noexcept;

    /// Topology snapshot without merging — used when STL merge experiment skips `MergeCoplanarFaces`.
    void CollectCoplanarMergeTopology(Solid *solid, MergeCoplanarDiagnostics *diagnosticsOut);

    /// After topology change: refresh `facesAfter` + post-merge edge histogram in `diag` (baseline fields untouched).
    void CompleteCoplanarMergeDiagnostics(Solid *solid, MergeCoplanarDiagnostics *diagnosticsOut);

    /// Initialize diagnostics before a merge step (baseline face/edge histogram, tolerances).
    void RecordCoplanarDiagnosticsBaselineForMerge(Solid *solid, MergeCoplanarDiagnostics *diagnosticsOut);

    void MergeCoplanarFaces(
        Solid *solid,
        MergeCoplanarDiagnostics *diagnosticsOut = nullptr,
        const SceneProgressCallback *progress = nullptr);

    /// Deep-copy the entire B-rep graph: points, curves, edges, faces, solids. The returned scene
    /// owns its own topology with no pointer aliasing back into `*this`.
    ///
    /// When `outFaceRemap` is non-null, fills `(*outFaceRemap)[srcFace] = clonedFace` for every
    /// face that was successfully cloned (omits faces dropped during clone).
    std::unique_ptr<Scene> Clone(std::unordered_map<const Face *, Face *> *outFaceRemap = nullptr) const;

    std::unordered_set<uint32_t> renderBuffer;
    std::unordered_set<uint32_t> lockedBuffer;
};