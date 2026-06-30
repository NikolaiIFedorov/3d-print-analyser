#include "Analysis.hpp"
#include "Analysis/OverhangDetector/OverhangDetector.hpp"
#include "Analysis/SharpCorner/SharpCorner.hpp"
#include "Analysis/Instability/Instability.hpp"
#include "Analysis/LayerDifference/LayerDifference.hpp"
#include "utils/Slice.hpp"
#include "utils/log.hpp"

#include <BRepClass3d_SolidClassifier.hxx>
#include <gp_Pnt.hxx>

#include <algorithm>
#include <chrono>
#include <mutex>

namespace
{
// Verbose per-pass timing is useful for focused profiling but expensive for normal interactive use.
inline constexpr bool kLogAnalysisTimingDetails = false;

static bool FaceHasLiveGeometry(const Face *face) noexcept
{
    return face != nullptr && face->HasGeometry();
}

/// Slices `solid` into correctly loop-separated cross-sections once, shared across every
/// detector for this analysis pass (replaces each detector independently calling
/// `Slice::Range`/`Slice::At`).
static std::vector<SlicedLayer> SliceSolidIntoLoops(const Solid &solid, double layerHeight)
{
    std::vector<SlicedLayer> layers;
    if (layerHeight <= 0.0)
        return layers;

    const ZBounds zb = Slice::GetZBounds(&solid);
    if (zb.zMax - zb.zMin < layerHeight)
        return layers;

    // Reused across layers — classifier setup from the solid's topology is the expensive step.
    BRepClass3d_SolidClassifier classifier(solid.occtShape);

    for (double z = zb.zMin + layerHeight; z < zb.zMax; z += layerHeight)
    {
        SlicedLayer layer;
        layer.z = z;
        layer.loops = Slice::ExtractLoops(Slice::At(&solid, z));

        // Override nesting-depth isHole with a solid point-in-solid test. The centroid of each
        // loop's boundary points approximates the interior of what the loop encloses:
        // — solid interior → the loop is an outer boundary (isHole = false)
        // — void interior  → the loop bounds a physical hole (isHole = true)
        // This correctly handles nested solid protrusions (e.g. overhang face sections that
        // happen to be geometrically inside the base outline), which nesting-depth alone
        // misclassifies as holes.
        for (auto &loop : layer.loops)
        {
            glm::dvec3 centroid{0.0, 0.0, 0.0};
            for (const auto &p : loop.ring)
                centroid += p;
            centroid /= static_cast<double>(loop.ring.size());

            classifier.Perform(gp_Pnt(centroid.x, centroid.y, centroid.z), 1e-4);
            const TopAbs_State state = classifier.State();
            loop.isHole = (state != TopAbs_IN);
        }

        layers.push_back(std::move(layer));
    }
    return layers;
}
} // namespace

void PruneDefunctAnalysisResults(AnalysisResults &results, const Scene *scene) noexcept
{
    (void)scene;
    for (auto &entry : results.faceFlawRanges)
    {
        auto &flaws = entry.second;
        flaws.erase(std::remove_if(flaws.begin(), flaws.end(),
                                   [](const FaceFlaw &ff) { return !FaceHasLiveGeometry(ff.face); }),
                    flaws.end());
    }
}

Analysis &Analysis::Instance()
{
    static Analysis instance;
    return instance;
}

void Analysis::AddSolidAnalysis(std::unique_ptr<ISolidAnalysis> analysis)
{
    std::lock_guard<std::mutex> lock(pipelineMutex);
    solidAnalyses.push_back(std::shared_ptr<ISolidAnalysis>(std::move(analysis)));
}

std::vector<FaceFlaw> Analysis::FlawSolid(const Solid *solid) const
{
    std::vector<std::shared_ptr<ISolidAnalysis>> localSolidAnalyses;
    float layerHeight;
    {
        std::lock_guard<std::mutex> lock(pipelineMutex);
        localSolidAnalyses = solidAnalyses;
        layerHeight = layerHeightForSlicing;
    }

    const std::vector<SlicedLayer> layers = SliceSolidIntoLoops(*solid, static_cast<double>(layerHeight));

    std::vector<FaceFlaw> allFlaws;
    for (const auto &analysis : localSolidAnalyses)
    {
        auto flaws = analysis->Analyze(solid, layers);
        allFlaws.insert(allFlaws.end(), flaws.begin(), flaws.end());
    }
    return allFlaws;
}

std::vector<SlicedLayer> Analysis::SliceSolidForDebug(const Solid *solid) const
{
    if (solid == nullptr)
        return {};
    float layerHeight;
    {
        std::lock_guard<std::mutex> lock(pipelineMutex);
        layerHeight = layerHeightForSlicing;
    }
    return SliceSolidIntoLoops(*solid, static_cast<double>(layerHeight));
}

void Analysis::Clear()
{
    std::lock_guard<std::mutex> lock(pipelineMutex);
    solidAnalyses.clear();
}

void Analysis::RebuildDefaultAnalyzers(float overhangAngle, float layerHeight, float sharpCornerThreshold,
                                       float instabilityMinWidth, float layerDifferenceMaxAreaDelta)
{
    std::lock_guard<std::mutex> lock(pipelineMutex);
    solidAnalyses.clear();
    layerHeightForSlicing = layerHeight;
    solidAnalyses.push_back(std::make_shared<OverhangDetector>(static_cast<double>(layerHeight),
                                                                static_cast<double>(overhangAngle)));
    solidAnalyses.push_back(
        std::make_shared<SharpCorner>(static_cast<double>(layerHeight), static_cast<double>(sharpCornerThreshold)));
    solidAnalyses.push_back(
        std::make_shared<Instability>(static_cast<double>(layerHeight), static_cast<double>(instabilityMinWidth)));
    solidAnalyses.push_back(std::make_shared<LayerDifference>(static_cast<double>(layerHeight),
                                                               static_cast<double>(layerDifferenceMaxAreaDelta)));
}

uint64_t Analysis::CountAnalyzeSteps(const Scene *scene) const
{
    if (scene == nullptr)
        return 0;

    size_t nSolidAnalyses = 0;
    {
        std::lock_guard<std::mutex> lock(pipelineMutex);
        nSolidAnalyses = solidAnalyses.size();
    }

    // One slicing step per solid, plus one analyzing step per (solid, detector) pair.
    const uint64_t slicingUnits = scene->solids.size();
    const uint64_t analyzingUnits = scene->solids.size() * static_cast<uint64_t>(nSolidAnalyses);

    return slicingUnits + analyzingUnits;
}

AnalysisResults Analysis::AnalyzeScene(const Scene *scene, const AnalyzeSceneReporter *reporter) const
{
    std::vector<std::shared_ptr<ISolidAnalysis>> localSolidAnalyses;
    float layerHeight;
    {
        std::lock_guard<std::mutex> lock(pipelineMutex);
        localSolidAnalyses = solidAnalyses;
        layerHeight = layerHeightForSlicing;
    }

    using Clock = std::chrono::steady_clock;
    auto elapsedMs = [](const Clock::time_point &start, const Clock::time_point &end) -> double
    {
        return std::chrono::duration<double, std::milli>(end - start).count();
    };

    AnalysisResults results;
    const Clock::time_point tSceneStart = Clock::now();

    std::vector<double> solidAnalyzerMs(localSolidAnalyses.size(), 0.0);
    double totalSlicingMs = 0.0;
    double totalAnalyzingMs = 0.0;

    uint64_t progressStepInScene = 0;
    auto bump = [&](uint32_t phaseId)
    {
        if (reporter)
        {
            ++progressStepInScene;
            (*reporter)(phaseId, progressStepInScene);
        }
    };

    for (size_t solidIndex = 0; solidIndex < scene->solids.size(); ++solidIndex)
    {
        const Solid &solid = scene->solids[solidIndex];
        const Clock::time_point tSolidStart = Clock::now();

        const Clock::time_point tSliceStart = Clock::now();
        const std::vector<SlicedLayer> layers = SliceSolidIntoLoops(solid, static_cast<double>(layerHeight));
        totalSlicingMs += elapsedMs(tSliceStart, Clock::now());
        bump(AnalysisUiPhase::Slicing);

        const Clock::time_point tAnalyzeStart = Clock::now();
        std::vector<FaceFlaw> allSolidFlaws;
        for (size_t i = 0; i < localSolidAnalyses.size(); ++i)
        {
            const Clock::time_point tStart = Clock::now();
            auto flaws = localSolidAnalyses[i]->Analyze(&solid, layers);
            const Clock::time_point tEnd = Clock::now();
            solidAnalyzerMs[i] += elapsedMs(tStart, tEnd);
            allSolidFlaws.insert(allSolidFlaws.end(), flaws.begin(), flaws.end());
            bump(AnalysisUiPhase::Analyzing);
        }
        results.faceFlawRanges[&solid] = std::move(allSolidFlaws);
        totalAnalyzingMs += elapsedMs(tAnalyzeStart, Clock::now());

        if constexpr (kLogAnalysisTimingDetails)
        {
            const double solidMs = elapsedMs(tSolidStart, Clock::now());
            LOG_INFO("Analysis solid", solidIndex, "faces", solid.faces.size(), "elapsedMs", solidMs);
        }
    }

    if constexpr (kLogAnalysisTimingDetails)
    {
        const double sceneMs = elapsedMs(tSceneStart, Clock::now());
        LOG_INFO("Analysis scene totalMs", sceneMs, "solids", scene->solids.size());
        LOG_INFO("Analysis stage slicingMs", totalSlicingMs, "analyzingMs", totalAnalyzingMs);
        for (size_t i = 0; i < solidAnalyzerMs.size(); ++i)
            LOG_INFO("Analysis solidAnalyzer", i, "ms", solidAnalyzerMs[i]);
    }

    return results;
}
