#pragma once

#include <unordered_set>
#include "Goemetry/AllGeometry.hpp"
#include "Id.hpp"

class Scene
{
public:
    std::vector<Point> points;
    std::unordered_map<uint32_t, uint32_t> pointIdToIndex;
    uint32_t CreatePoint(const glm::dvec3 &position);
    const Point *GetPoint(uint32_t id) const;

    std::vector<Edge> edges;
    std::unordered_map<uint32_t, uint32_t> edgeIdToIndex;
    uint32_t CreateLineEdge(uint32_t startPointId, uint32_t endPointId, uint32_t curveId = 0);
    uint32_t CreatePolylineEdge(uint32_t startPointId, uint32_t endPointId, const std::vector<uint32_t> &bridgePointIds);
    const Edge *GetEdge(uint32_t id) const;

    std::vector<Curve> curves;
    std::unordered_map<uint32_t, uint32_t> curveIdToIndex;
    uint32_t CreateArcCurve(glm::dvec3 centerPoint, double radius);
    uint32_t CreateNURBSCurve(const tinynurbs::RationalCurve3d &nurbs);
    const Curve *GetCurve(uint32_t id) const;
    Curve *GetCurve(uint32_t id);

    std::vector<Face> faces;
    std::unordered_map<uint32_t, uint32_t> faceIdToIndex;
    uint32_t CreatePlanarFace(const std::vector<std::vector<uint32_t>> &edgeLoops);
    uint32_t CreateNURBSFace(const std::vector<std::vector<uint32_t>> &edgeLoops, const tinynurbs::RationalSurface3d &nurbs);
    const Face *GetFace(uint32_t id) const;
    Face *GetFace(uint32_t id);

    std::vector<Solid> solids;
    std::unordered_map<uint32_t, uint32_t> solidIdToIndex;
    uint32_t CreateSolid(const std::vector<uint32_t> &faces);
    const Solid *GetSolid(uint32_t id) const;

    std::unordered_map<uint32_t, std::unordered_set<uint32_t>> fromIdToDependancies;
    bool DeletePoint(uint32_t id);
    bool DeleteCurve(uint32_t id);
    bool DeleteEdge(uint32_t id);
    bool DeleteFace(uint32_t id);
    bool DeleteSolid(uint32_t id);

    std::unordered_set<uint32_t> renderBuffer;
    std::unordered_set<uint32_t> lockedBuffer;

private:
    void RegisterDependency(uint32_t formId, uint32_t dependancyId);
    void UnregisterDependency(uint32_t formId, uint32_t dependancyId);
    void UnregisterDependency(uint32_t formId);
};