#pragma once
#include <vector>
#include <memory>
#include <optional>

#include <glm/glm.hpp>

#include "scene/scene.hpp"
#include "AnalysisTypes.hpp"

class IFaceAnalysis
{
public:
    virtual ~IFaceAnalysis() = default;
    virtual std::optional<Flaw> Analyze(const Face *face) const = 0;
};

class ISolidAnalysis
{
public:
    virtual ~ISolidAnalysis() = default;
    virtual std::vector<FaceFlaw> Analyze(const Solid *solid, std::optional<ZBounds> bounds = std::nullopt,
                                          std::vector<BridgeSurface> *bridgeSurfaces = nullptr) const = 0;
};

class IEdgeAnalysis
{
public:
    virtual ~IEdgeAnalysis() = default;
    virtual std::vector<EdgeFlaw> Analyze(const Solid *solid) const = 0;
};

class Analysis
{
public:
    static Analysis &Instance();

    void AddFaceAnalysis(std::unique_ptr<IFaceAnalysis> analysis);
    void AddSolidAnalysis(std::unique_ptr<ISolidAnalysis> analysis);
    void AddEdgeAnalysis(std::unique_ptr<IEdgeAnalysis> analysis);

    Flaw FlawFace(const Face *face) const;
    std::vector<FaceFlaw> FlawSolid(const Solid *solid, std::vector<BridgeSurface> *bridgeSurfaces = nullptr) const;
    std::vector<EdgeFlaw> FlawEdges(const Solid *solid) const;

    AnalysisResults AnalyzeScene(const Scene *scene) const;

private:
    std::vector<std::unique_ptr<IFaceAnalysis>> faceAnalyses;
    std::vector<std::unique_ptr<ISolidAnalysis>> solidAnalyses;
    std::vector<std::unique_ptr<IEdgeAnalysis>> edgeAnalyses;
};