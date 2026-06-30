#pragma once
#include <vector>
#include <memory>
#include <mutex>
#include <functional>
#include <cstdint>

#include <glm/glm.hpp>

#include "scene/scene.hpp"
#include "AnalysisTypes.hpp"

/// IDs for coarse UI progress labels during `AnalyzeScene` (worker thread reports these).
/// Every detector is now slicing-based, so there's no separate face-pass/edge-pass distinction
/// left — just slicing each solid, then running the detector stack over the shared result.
namespace AnalysisUiPhase
{
inline constexpr uint32_t Slicing = 1;
inline constexpr uint32_t Analyzing = 2;
}

/// All manufacturability detectors operate on one solid's already-sliced, loop-separated
/// cross-sections (`Analysis::AnalyzeScene` slices each solid exactly once via
/// `Slice::ExtractLoops` and shares the result across every detector) rather than each detector
/// re-deriving its own slice/loop data independently.
class ISolidAnalysis
{
public:
    virtual ~ISolidAnalysis() = default;
    virtual std::vector<FaceFlaw> Analyze(const Solid *solid, const std::vector<SlicedLayer> &layers) const = 0;
};

class Analysis
{
public:
    static Analysis &Instance();

    void AddSolidAnalysis(std::unique_ptr<ISolidAnalysis> analysis);

    std::vector<FaceFlaw> FlawSolid(const Solid *solid) const;

    /// Re-slices `solid` at the current layer height for debug visualization (e.g. showing the
    /// raw cross-sections every detector operates on). Independent of `AnalyzeScene`'s cached results.
    std::vector<SlicedLayer> SliceSolidForDebug(const Solid *solid) const;

    /// Progress `reporter(phaseId, stepIndex)` emits monotonic `stepIndex` starting at 1 per worker session
    /// (caller adds queue offset externally). Omit or pass `nullptr` to skip.
    using AnalyzeSceneReporter = std::function<void(uint32_t phaseId, uint64_t stepIndex)>;
    /// Runs read-only passes over `scene`. The analyzer list is **copied under `pipelineMutex`** and
    /// released before any per-solid work so workers do not hold that lock across heavy geometry
    /// (avoids deadlocks vs UI calling `RebuildDefaultAnalyzers` / `Clear`).
    AnalysisResults AnalyzeScene(const Scene *scene, const AnalyzeSceneReporter *reporter = nullptr) const;
    [[nodiscard]] uint64_t CountAnalyzeSteps(const Scene *scene) const;

    void Clear();

    /// Atomically replaces the built-in analyzer stack (single lock — used when UI thresholds change).
    void RebuildDefaultAnalyzers(float overhangAngle, float layerHeight, float notEnoughSpaceThreshold,
                                 float instabilityMinWidth, float layerDifferenceMaxAreaDelta);

private:
    mutable std::mutex pipelineMutex;
    std::vector<std::shared_ptr<ISolidAnalysis>> solidAnalyses;
    float layerHeightForSlicing = 0.2f;
};
