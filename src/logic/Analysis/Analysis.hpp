#pragma once
#include <vector>
#include <memory>
#include <mutex>
#include <optional>
#include <functional>
#include <cstdint>

#include <glm/glm.hpp>

#include "scene/scene.hpp"
#include "AnalysisTypes.hpp"

/// IDs for coarse UI progress labels during `AnalyzeScene` (worker thread reports these).
namespace AnalysisUiPhase
{
inline constexpr uint32_t FacePassesSolidFaces = 1;
inline constexpr uint32_t FacePassesLooseFaces = 2;
inline constexpr uint32_t SolidAnalyzers = 3;
inline constexpr uint32_t EdgeAnalyzers = 4;
}

class IFaceAnalysis
{
public:
    virtual ~IFaceAnalysis() = default;
    virtual std::optional<FaceFlawKind> Analyze(const Face *face) const = 0;
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

    FaceFlawKind FlawFace(const Face *face) const;
    std::vector<FaceFlaw> FlawSolid(const Solid *solid, std::vector<BridgeSurface> *bridgeSurfaces = nullptr) const;
    std::vector<EdgeFlaw> FlawEdges(const Solid *solid) const;

    /// Progress `reporter(phaseId, stepIndex)` emits monotonic `stepIndex` starting at 1 per worker session
    /// (caller adds queue offset externally). Omit or pass `nullptr` to skip.
    using AnalyzeSceneReporter = std::function<void(uint32_t phaseId, uint64_t stepIndex)>;
    /// Runs read-only passes over `scene`. The analyzer list is **copied under `pipelineMutex`** and
    /// released before any per-face/solid work so workers do not hold that lock across heavy geometry
    /// (avoids deadlocks vs UI calling `RebuildDefaultAnalyzers` / `Clear`).
    AnalysisResults AnalyzeScene(const Scene *scene, const AnalyzeSceneReporter *reporter = nullptr) const;
    [[nodiscard]] uint64_t CountAnalyzeSteps(const Scene *scene) const;

    void Clear();

    /// Atomically replaces the built-in analyzer stack (single lock — used when UI thresholds change).
    void RebuildDefaultAnalyzers(float overhangAngle, float layerHeight, float minFeatureSize, float thinMinWidth,
                                 float sharpCornerAngle);

private:
    mutable std::mutex pipelineMutex;
    std::vector<std::shared_ptr<IFaceAnalysis>> faceAnalyses;
    std::vector<std::shared_ptr<ISolidAnalysis>> solidAnalyses;
    std::vector<std::shared_ptr<IEdgeAnalysis>> edgeAnalyses;
};