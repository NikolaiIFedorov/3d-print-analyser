#pragma once

#include <string>
#include <cstddef>
#include <cstdint>

#include "ImportProgress.hpp"
#include "scene/scene.hpp"

class Scene;

struct STLImportStats
{
    bool isBinary = false;
    uint32_t triangleCount = 0;
    std::size_t uniquePoints = 0;
    std::size_t faces = 0;
    double parseMs = 0.0;
    double mergeMs = 0.0;
    double totalMs = 0.0;

    /// Set after a solid exists and merge/topology instrumentation ran (STL path only).
    bool hasMergeDiagnostics = false;
    MergeCoplanarDiagnostics mergeDiagnostics;
};

class STLImport
{
public:
    static bool Import(
        const std::string &filePath,
        Scene *scene,
        STLImportStats *stats = nullptr,
        const ImportProgressCallback *progress = nullptr);
};
