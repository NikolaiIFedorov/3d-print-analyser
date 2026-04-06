#include "FileImport.hpp"
#include "utils/log.hpp"

void FileImport::OpenFileDialog(SDL_Window *window, FileCallback onFileSelected)
{
    static const SDL_DialogFileFilter filters[] = {
        {"3D Files", "stl;step;stp;3mf;obj;ply"},
        {"STL", "stl"},
        {"STEP", "step;stp"},
        {"3MF", "3mf"},
        {"OBJ", "obj"},
        {"PLY", "ply"},
    };

    auto *cb = new FileCallback(std::move(onFileSelected));

    SDL_ShowOpenFileDialog(
        [](void *userdata, const char *const *filelist, int)
        {
            auto *callback = static_cast<FileCallback *>(userdata);
            if (filelist)
            {
                for (int i = 0; filelist[i]; ++i)
                {
                    (*callback)(std::string(filelist[i]));
                }
            }
            else
            {
                LOG_DESC("File selection cancelled")
            }
            delete callback;
        },
        cb, window, filters, 6, nullptr, true);
}
