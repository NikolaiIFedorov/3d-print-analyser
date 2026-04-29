#pragma once

#include <string>
#include <cstddef>
#include <cstdint>

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
};

class STLImport
{
public:
    static bool Import(const std::string &filePath, Scene *scene, STLImportStats *stats = nullptr);
};
