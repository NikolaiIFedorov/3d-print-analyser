#include "Analysis.hpp"
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
    std::lock_guard<std::recursive_mutex> lock(pipelineMutex);
    faceAnalyses.push_back(std::shared_ptr<IFaceAnalysis>(std::move(analysis)));
}

void Analysis::AddSolidAnalysis(std::unique_ptr<ISolidAnalysis> analysis)
{
    std::lock_guard<std::recursive_mutex> lock(pipelineMutex);
    solidAnalyses.push_back(std::shared_ptr<ISolidAnalysis>(std::move(analysis)));
}

void Analysis::AddEdgeAnalysis(std::unique_ptr<IEdgeAnalysis> analysis)
{
    std::lock_guard<std::recursive_mutex> lock(pipelineMutex);
    edgeAnalyses.push_back(std::shared_ptr<IEdgeAnalysis>(std::move(analysis)));
}

FaceFlawKind Analysis::FlawFace(const Face *face) const
{
    std::lock_guard<std::recursive_mutex> lock(pipelineMutex);
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
    std::lock_guard<std::recursive_mutex> lock(pipelineMutex);
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
    std::lock_guard<std::recursive_mutex> lock(pipelineMutex);
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
    std::lock_guard<std::recursive_mutex> lock(pipelineMutex);
    faceAnalyses.clear();
    solidAnalyses.clear();
    edgeAnalyses.clear();
}

AnalysisResults Analysis::AnalyzeScene(const Scene *scene) const
{
    std::vector<std::shared_ptr<IFaceAnalysis>> localFaceAnalyses;
    std::vector<std::shared_ptr<ISolidAnalysis>> localSolidAnalyses;
    std::vector<std::shared_ptr<IEdgeAnalysis>> localEdgeAnalyses;
    {
        std::lock_guard<std::recursive_mutex> lock(pipelineMutex);
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
