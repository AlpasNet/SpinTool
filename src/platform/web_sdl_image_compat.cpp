#include "SDL3/SDL_image.h"

#include "platform/web_platform.h"

#include <cstring>
#include <filesystem>

extern "C"
{
    SDL_Surface* SDLCALL IMG_Load(const char* file)
    {
        if (file == nullptr)
        {
            SDL_SetError("IMG_Load received a null path");
            return nullptr;
        }
        return SDL_LoadSurface(file);
    }

    SDL_Surface* SDLCALL IMG_LoadTyped_IO(
        SDL_IOStream* src,
        bool closeio,
        const char* type
    )
    {
        if (src == nullptr)
        {
            SDL_SetError("IMG_LoadTyped_IO received a null stream");
            return nullptr;
        }

        if (type == nullptr || SDL_strcasecmp(type, "PNG") == 0)
        {
            return SDL_LoadPNG_IO(src, closeio);
        }

        if (closeio)
        {
            SDL_CloseIO(src);
        }
        SDL_SetError("SpinTool Web only supports PNG for in-memory image imports");
        return nullptr;
    }

    SDL_Texture* SDLCALL IMG_LoadTexture(SDL_Renderer* renderer, const char* file)
    {
        if (renderer == nullptr)
        {
            SDL_SetError("IMG_LoadTexture received a null renderer");
            return nullptr;
        }

        SDL_Surface* surface = IMG_Load(file);
        if (surface == nullptr)
        {
            return nullptr;
        }

        SDL_Texture* texture = SDL_CreateTextureFromSurface(renderer, surface);
        SDL_DestroySurface(surface);
        return texture;
    }

    bool SDLCALL IMG_SavePNG(SDL_Surface* surface, const char* file)
    {
        if (surface == nullptr || file == nullptr)
        {
            SDL_SetError("IMG_SavePNG received an invalid argument");
            return false;
        }

        if (!SDL_SavePNG(surface, file))
        {
            return false;
        }

        (void)spintool::web::DownloadFile(
            std::filesystem::path{file},
            "image/png"
        );
        return true;
    }
}
