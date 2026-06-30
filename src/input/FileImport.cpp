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

    struct DialogContext
    {
        FileCallback callback;
        SDL_Window *window;
    };
    auto *ctx = new DialogContext{std::move(onFileSelected), window};

    SDL_ShowOpenFileDialog(
        [](void *userdata, const char *const *filelist, int)
        {
            auto *context = static_cast<DialogContext *>(userdata);
            if (filelist)
            {
                for (int i = 0; filelist[i]; ++i)
                {
                    context->callback(std::string(filelist[i]));
                }
            }
            else
            {
                LOG_DESC("File selection cancelled")
            }
            // The native open panel steals key-window status; without explicitly raising
            // the window back here, macOS leaves it non-key after the panel closes, so the
            // compositor won't present newly-swapped frames until a real focus-restoring
            // input event (e.g. a mouse move) arrives — even though our render loop keeps
            // swapping buffers in the background.
            SDL_RaiseWindow(context->window);
            delete context;
        },
        ctx, window, filters, 6, nullptr, true);
}
