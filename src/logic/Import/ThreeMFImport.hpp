#pragma once

#include <string>

#include "ImportProgress.hpp"

class Scene;

class ThreeMFImport
{
public:
    static bool Import(
        const std::string &filePath,
        Scene *scene,
        const ImportProgressCallback *progress = nullptr);
};
