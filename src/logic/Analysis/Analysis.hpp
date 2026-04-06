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
    virtual std::vector<Layer> Analyze(const Solid *solid, std::optional<ZBounds> bounds = std::nullopt) const = 0;
};

class Analysis
{
public:
    static Analysis &Instance();

    void AddFaceAnalysis(std::unique_ptr<IFaceAnalysis> analysis);
    void AddSolidAnalysis(std::unique_ptr<ISolidAnalysis> analysis);

    Flaw FlawFace(const Face *face) const;
    std::vector<Layer> FlawSolid(const Solid *solid) const;

    AnalysisResults AnalyzeScene(const Scene *scene) const;

private:
    std::vector<std::unique_ptr<IFaceAnalysis>> faceAnalyses;
    std::vector<std::unique_ptr<ISolidAnalysis>> solidAnalyses;
};