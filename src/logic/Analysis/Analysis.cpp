#include "Analysis.hpp"
#include "utils/Slice.hpp"

Analysis &Analysis::Instance()
{
    static Analysis instance;
    return instance;
}

void Analysis::AddFaceAnalysis(std::unique_ptr<IFaceAnalysis> analysis)
{
    faceAnalyses.push_back(std::move(analysis));
}

void Analysis::AddSolidAnalysis(std::unique_ptr<ISolidAnalysis> analysis)
{
    solidAnalyses.push_back(std::move(analysis));
}

Flaw Analysis::FlawFace(const Face *face) const
{
    for (const auto &analysis : faceAnalyses)
    {
        auto result = analysis->Analyze(face);
        if (result.has_value())
            return result.value();
    }

    return Flaw::NONE;
}

std::vector<Layer> Analysis::FlawSolid(const Solid *solid) const
{
    std::vector<Layer> allLayers;
    for (const auto &analysis : solidAnalyses)
    {
        auto layers = analysis->Analyze(solid);
        allLayers.insert(allLayers.end(), layers.begin(), layers.end());
    }
    return allLayers;
}
