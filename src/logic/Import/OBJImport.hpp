#pragma once

#include <string>

class Scene;

class OBJImport
{
public:
    static bool Import(const std::string &filePath, Scene *scene);
};
