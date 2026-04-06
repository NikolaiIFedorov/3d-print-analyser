#pragma once

#include <SDL3/SDL.h>
#include <functional>
#include <string>

class FileImport
{
public:
    using FileCallback = std::function<void(const std::string &)>;
    static void OpenFileDialog(SDL_Window *window, FileCallback onFileSelected);
};
