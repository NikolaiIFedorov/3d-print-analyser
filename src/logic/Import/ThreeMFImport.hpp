#pragma once

#include <string>

class Scene;

class ThreeMFImport
{
public:
    static bool Import(const std::string &filePath, Scene *scene);
};
