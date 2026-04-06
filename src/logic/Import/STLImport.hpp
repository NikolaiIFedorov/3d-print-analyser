#pragma once

#include <string>

class Scene;

class STLImport
{
public:
    static bool Import(const std::string &filePath, Scene *scene);
};
