#include "Analysis.hpp"
#include "Analysis/Overhang/Overhang.hpp"
#include "Analysis/SharpCorner/SharpCorner.hpp"
#include "Analysis/SmallFeature/SmallFeature.hpp"
#include "Analysis/ThinSection/ThinSection.hpp"
#include "utils/Slice.hpp"
#include "utils/log.hpp"

#include <chrono>
#include <mutex>

namespace
{
// Verbose per-pass timing is useful for focused profiling but expensive for normal interactive use.
inline constexpr bool kLogAnalysisTimingDetails = false;
}

Analysis &Analysis::Instance()
{
    static Analysis instance;
    return instance;
}

void Analysis::AddFaceAnalysis(std::unique_ptr<IFaceAnalysis> analysis)
{
    std::lock_guard<std::mutex> lock(pipelineMutex);
    faceAnalyses.push_back(std::shared_ptr<IFaceAnalysis>(std::move(analysis)));
}

void Analysis::AddSolidAnalysis(std::unique_ptr<ISolidAnalysis> analysis)
{
    std::lock_guard<std::mutex> lock(pipelineMutex);
    solidAnalyses.push_back(std::shared_ptr<ISolidAnalysis>(std::move(analysis)));
}

void Analysis::AddEdgeAnalysis(std::unique_ptr<IEdgeAnalysis> analysis)
{
    std::lock_guard<std::mutex> lock(pipelineMutex);
    edgeAnalyses.push_back(std::shared_ptr<IEdgeAnalysis>(std::move(analysis)));
}

FaceFlawKind Analysis::FlawFace(const Face *face) const
{
    std::lock_guard<std::mutex> lock(pipelineMutex);
    for (const auto &analysis : faceAnalyses)
    {
        auto result = analysis->Analyze(face);
        if (result.has_value())
            return result.value();
    }

    return FaceFlawKind::NONE;
}

std::vector<FaceFlaw> Analysis::FlawSolid(const Solid *solid, std::vector<BridgeSurface> *bridgeSurfaces) const
{
    std::lock_guard<std::mutex> lock(pipelineMutex);
    ZBounds bounds = Slice::GetZBounds(solid);

    std::vector<FaceFlaw> allFlaws;
    for (const auto &analysis : solidAnalyses)
    {
        auto flaws = analysis->Analyze(solid, bounds, bridgeSurfaces);
        allFlaws.insert(allFlaws.end(), flaws.begin(), flaws.end());
    }
    return allFlaws;
}

std::vector<EdgeFlaw> Analysis::FlawEdges(const Solid *solid) const
{
    std::lock_guard<std::mutex> lock(pipelineMutex);
    std::vector<EdgeFlaw> allEdgeFlaws;
    for (const auto &analysis : edgeAnalyses)
    {
        auto flaws = analysis->Analyze(solid);
        allEdgeFlaws.insert(allEdgeFlaws.end(), flaws.begin(), flaws.end());
    }
    return allEdgeFlaws;
}

void Analysis::Clear()
{
    std::lock_guard<std::mutex> lock(pipelineMutex);
    faceAnalyses.clear();
    solidAnalyses.clear();
    edgeAnalyses.clear();
}

void Analysis::RebuildDefaultAnalyzers(float overhangAngle, float layerHeight, float minFeatureSize, float thinMinWidth,
                                       float sharpCornerAngle)
{
    std::lock_guard<std::mutex> lock(pipelineMutex);
    faceAnalyses.clear();
    solidAnalyses.clear();
    edgeAnalyses.clear();
    faceAnalyses.push_back(std::make_shared<Overhang>(static_cast<double>(overhangAngle)));
    solidAnalyses.push_back(std::make_shared<SmallFeature>(static_cast<double>(layerHeight), static_cast<double>(minFeatureSize)));
    solidAnalyses.push_back(std::make_shared<ThinSection>(static_cast<double>(layerHeight), static_cast<double>(thinMinWidth)));
    edgeAnalyses.push_back(std::make_shared<SharpCorner>(static_cast<double>(sharpCornerAngle)));
}

uint64_t Analysis::CountAnalyzeSteps(const Scene *scene) const
{
    if (scene == nullptr)
        return 0;

    size_t nSolidAnalyses = 0;
    size_t nEdgeAnalyses = 0;
    {
        std::lock_guard<std::mutex> lock(pipelineMutex);
        nSolidAnalyses = solidAnalyses.size();
        nEdgeAnalyses = edgeAnalyses.size();
    }

    uint64_t faceUnits = 0;
    for (const Solid &solid : scene->solids)
        faceUnits += solid.faces.size();
    faceUnits += scene->faces.size();

    const uint64_t solidUnits = scene->solids.size() * static_cast<uint64_t>(nSolidAnalyses);
    const uint64_t edgeUnits = scene->solids.size() * static_cast<uint64_t>(nEdgeAnalyses);

    return faceUnits + solidUnits + edgeUnits;
}

AnalysisResults Analysis::AnalyzeScene(const Scene *scene, const AnalyzeSceneReporter *reporter) const
{
    std::vector<std::shared_ptr<IFaceAnalysis>> localFaceAnalyses;
    std::vector<std::shared_ptr<ISolidAnalysis>> localSolidAnalyses;
    std::vector<std::shared_ptr<IEdgeAnalysis>> localEdgeAnalyses;
    {
        std::lock_guard<std::mutex> lock(pipelineMutex);
        localFaceAnalyses = faceAnalyses;
        localSolidAnalyses = solidAnalyses;
        localEdgeAnalyses = edgeAnalyses;
    }

    using Clock = std::chrono::steady_clock;
    auto elapsedMs = [](const Clock::time_point &start, const Clock::time_point &end) -> double
    {
        return std::chrono::duration<double, std::milli>(end - start).count();
    };

    AnalysisResults results;
    const Clock::time_point tSceneStart = Clock::now();

    std::vector<double> faceAnalyzerMs(localFaceAnalyses.size(), 0.0);
    std::vector<double> solidAnalyzerMs(localSolidAnalyses.size(), 0.0);
    std::vector<double> edgeAnalyzerMs(localEdgeAnalyses.size(), 0.0);
    double totalFacePassMs = 0.0;
    double totalSolidPassMs = 0.0;
    double totalEdgePassMs = 0.0;

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

        const Clock::time_point tFacePassStart = Clock::now();
        for (const Face *face : solid.faces)
        {
            FaceFlawKind faceFlaw = FaceFlawKind::NONE;
            for (size_t i = 0; i < localFaceAnalyses.size(); ++i)
            {
                const Clock::time_point tStart = Clock::now();
                auto result = localFaceAnalyses[i]->Analyze(face);
                const Clock::time_point tEnd = Clock::now();
                faceAnalyzerMs[i] += elapsedMs(tStart, tEnd);
                if (result.has_value())
                {
                    faceFlaw = result.value();
                    break;
                }
            }
            results.faceFlaws[face] = faceFlaw;
            bump(AnalysisUiPhase::FacePassesSolidFaces);
        }
        totalFacePassMs += elapsedMs(tFacePassStart, Clock::now());

        const Clock::time_point tSolidPassStart = Clock::now();
        ZBounds bounds = Slice::GetZBounds(&solid);
        std::vector<FaceFlaw> allSolidFlaws;
        for (size_t i = 0; i < localSolidAnalyses.size(); ++i)
        {
            const Clock::time_point tStart = Clock::now();
            auto flaws = localSolidAnalyses[i]->Analyze(&solid, bounds, &results.bridgeSurfaces[&solid]);
            const Clock::time_point tEnd = Clock::now();
            solidAnalyzerMs[i] += elapsedMs(tStart, tEnd);
            allSolidFlaws.insert(allSolidFlaws.end(), flaws.begin(), flaws.end());
            bump(AnalysisUiPhase::SolidAnalyzers);
        }
        results.faceFlawRanges[&solid] = std::move(allSolidFlaws);
        totalSolidPassMs += elapsedMs(tSolidPassStart, Clock::now());

        const Clock::time_point tEdgePassStart = Clock::now();
        std::vector<EdgeFlaw> allEdgeFlaws;
        for (size_t i = 0; i < localEdgeAnalyses.size(); ++i)
        {
            const Clock::time_point tStart = Clock::now();
            auto flaws = localEdgeAnalyses[i]->Analyze(&solid);
            const Clock::time_point tEnd = Clock::now();
            edgeAnalyzerMs[i] += elapsedMs(tStart, tEnd);
            allEdgeFlaws.insert(allEdgeFlaws.end(), flaws.begin(), flaws.end());
            bump(AnalysisUiPhase::EdgeAnalyzers);
        }
        results.edgeFlaws[&solid] = std::move(allEdgeFlaws);
        totalEdgePassMs += elapsedMs(tEdgePassStart, Clock::now());

        if constexpr (kLogAnalysisTimingDetails)
        {
            const double solidMs = elapsedMs(tSolidStart, Clock::now());
            LOG_INFO("Analysis solid", solidIndex, "faces", solid.faces.size(), "elapsedMs", solidMs);
        }
    }

    const Clock::time_point tLooseFaceStart = Clock::now();
    for (const Face &face : scene->faces)
    {
        FaceFlawKind faceFlaw = FaceFlawKind::NONE;
        for (size_t i = 0; i < localFaceAnalyses.size(); ++i)
        {
            const Clock::time_point tStart = Clock::now();
            auto result = localFaceAnalyses[i]->Analyze(&face);
            const Clock::time_point tEnd = Clock::now();
            faceAnalyzerMs[i] += elapsedMs(tStart, tEnd);
            if (result.has_value())
            {
                faceFlaw = result.value();
                break;
            }
        }
        results.faceFlaws[&face] = faceFlaw;
        bump(AnalysisUiPhase::FacePassesLooseFaces);
    }
    totalFacePassMs += elapsedMs(tLooseFaceStart, Clock::now());

    if constexpr (kLogAnalysisTimingDetails)
    {
        const double sceneMs = elapsedMs(tSceneStart, Clock::now());
        LOG_INFO("Analysis scene totalMs", sceneMs, "solids", scene->solids.size(), "looseFaces", scene->faces.size());
        LOG_INFO("Analysis stage faceMs", totalFacePassMs, "solidMs", totalSolidPassMs, "edgeMs", totalEdgePassMs);
        for (size_t i = 0; i < faceAnalyzerMs.size(); ++i)
            LOG_INFO("Analysis faceAnalyzer", i, "ms", faceAnalyzerMs[i]);
        for (size_t i = 0; i < solidAnalyzerMs.size(); ++i)
            LOG_INFO("Analysis solidAnalyzer", i, "ms", solidAnalyzerMs[i]);
        for (size_t i = 0; i < edgeAnalyzerMs.size(); ++i)
            LOG_INFO("Analysis edgeAnalyzer", i, "ms", edgeAnalyzerMs[i]);
    }

    return results;
}
