#include "Settings.hpp"
#include <SDL3/SDL.h>

std::filesystem::path Settings::DefaultPath()
{
    // SDL_GetPrefPath returns a heap-allocated C string — free with SDL_free.
    char *sdlPath = SDL_GetPrefPath("CAD_OpenGL", "CAD_OpenGL");
    std::filesystem::path result;
    if (sdlPath)
    {
        result = std::filesystem::path(sdlPath) / "settings.xml";
        SDL_free(sdlPath);
    }
    return result;
}
