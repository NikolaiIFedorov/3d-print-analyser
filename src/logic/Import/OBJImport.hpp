#pragma once

#include <string>

#include "ImportProgress.hpp"

class Scene;

class OBJImport
{
public:
    static bool Import(
        const std::string &filePath,
        Scene *scene,
        const ImportProgressCallback *progress = nullptr);
};
