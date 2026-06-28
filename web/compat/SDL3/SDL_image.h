#pragma once

#include <SDL3/SDL.h>

#ifdef __cplusplus
extern "C" {
#endif

SDL_DECLSPEC SDL_Surface* SDLCALL IMG_Load(const char* file);
SDL_DECLSPEC SDL_Surface* SDLCALL IMG_LoadTyped_IO(
    SDL_IOStream* src,
    bool closeio,
    const char* type
);
SDL_DECLSPEC SDL_Texture* SDLCALL IMG_LoadTexture(
    SDL_Renderer* renderer,
    const char* file
);
SDL_DECLSPEC bool SDLCALL IMG_SavePNG(SDL_Surface* surface, const char* file);

#ifdef __cplusplus
}
#endif
