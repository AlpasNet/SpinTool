#if defined(__EMSCRIPTEN__)

#include "SDL3/SDL_error.h"
#include "SDL3/SDL_image.h"
#include "SDL3/SDL_iostream.h"
#include "SDL3/SDL_pixels.h"
#include "SDL3/SDL_render.h"
#include "SDL3/SDL_surface.h"

#include <png.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>

namespace
{
    struct PngErrorState
    {
        std::array<char, 256> message{};
    };

    struct MemoryReader
    {
        const Uint8* data = nullptr;
        std::size_t size = 0;
        std::size_t offset = 0;
    };

    void PngErrorHandler(png_structp png, png_const_charp message)
    {
        auto* state = static_cast<PngErrorState*>(png_get_error_ptr(png));
        if (state != nullptr)
        {
            std::snprintf(
                state->message.data(),
                state->message.size(),
                "%s",
                message != nullptr ? message : "unknown libpng error"
            );
        }
        png_longjmp(png, 1);
    }

    void PngWarningHandler(png_structp, png_const_charp)
    {
        // Warnings do not invalidate otherwise decodable images.
    }

    void ReadFromMemory(png_structp png, png_bytep output, png_size_t byte_count)
    {
        auto* reader = static_cast<MemoryReader*>(png_get_io_ptr(png));
        if (reader == nullptr || output == nullptr ||
            byte_count > reader->size || reader->offset > reader->size - byte_count)
        {
            png_error(png, "unexpected end of PNG data");
            return;
        }

        std::memcpy(output, reader->data + reader->offset, byte_count);
        reader->offset += byte_count;
    }

    bool IsPngSignature(const Uint8* data, const std::size_t size)
    {
        return data != nullptr && size >= 8U && png_sig_cmp(
            const_cast<png_bytep>(reinterpret_cast<const png_byte*>(data)),
            0,
            8
        ) == 0;
    }

    SDL_Surface* LoadPngFromMemory(const Uint8* data, const std::size_t size)
    {
        if (!IsPngSignature(data, size))
        {
            SDL_SetError("The selected file is not a valid PNG image.");
            return nullptr;
        }

        PngErrorState error_state{};
        MemoryReader reader{data, size, 0U};
        png_structp png = png_create_read_struct(
            PNG_LIBPNG_VER_STRING,
            &error_state,
            PngErrorHandler,
            PngWarningHandler
        );
        if (png == nullptr)
        {
            SDL_SetError("libpng could not create its read context.");
            return nullptr;
        }

        png_infop info = png_create_info_struct(png);
        if (info == nullptr)
        {
            png_destroy_read_struct(&png, nullptr, nullptr);
            SDL_SetError("libpng could not create its image information context.");
            return nullptr;
        }

        SDL_Surface* surface = nullptr;
        SDL_Palette* palette = nullptr;
        png_bytep* rows = nullptr;

        if (setjmp(png_jmpbuf(png)) != 0)
        {
            if (rows != nullptr)
            {
                SDL_free(rows);
            }
            if (palette != nullptr)
            {
                SDL_DestroyPalette(palette);
            }
            if (surface != nullptr)
            {
                SDL_DestroySurface(surface);
            }
            png_destroy_read_struct(&png, &info, nullptr);
            SDL_SetError(
                "libpng could not decode the PNG: %s",
                error_state.message[0] != '\0'
                    ? error_state.message.data()
                    : "unknown error"
            );
            return nullptr;
        }

        png_set_read_fn(png, &reader, ReadFromMemory);
        png_read_info(png, info);

        const png_uint_32 png_width = png_get_image_width(png, info);
        const png_uint_32 png_height = png_get_image_height(png, info);
        const int colour_type = png_get_color_type(png, info);
        const int bit_depth = png_get_bit_depth(png, info);

        if (png_width == 0U || png_height == 0U ||
            png_width > static_cast<png_uint_32>(std::numeric_limits<int>::max()) ||
            png_height > static_cast<png_uint_32>(std::numeric_limits<int>::max()))
        {
            png_error(png, "invalid PNG dimensions");
        }

        const bool indexed = colour_type == PNG_COLOR_TYPE_PALETTE && bit_depth <= 8;
        if (indexed)
        {
            if (bit_depth < 8)
            {
                png_set_packing(png);
            }
            (void)png_set_interlace_handling(png);
            png_read_update_info(png, info);

            surface = SDL_CreateSurface(
                static_cast<int>(png_width),
                static_cast<int>(png_height),
                SDL_PIXELFORMAT_INDEX8
            );
            if (surface == nullptr)
            {
                png_error(png, SDL_GetError());
            }

            png_colorp png_palette = nullptr;
            int png_palette_size = 0;
            if (png_get_PLTE(png, info, &png_palette, &png_palette_size) == 0 ||
                png_palette == nullptr || png_palette_size <= 0)
            {
                png_error(png, "indexed PNG has no palette");
            }

            palette = SDL_CreatePalette(256);
            if (palette == nullptr)
            {
                png_error(png, SDL_GetError());
            }

            std::array<SDL_Color, 256> colours{};
            for (SDL_Color& colour : colours)
            {
                colour = SDL_Color{0U, 0U, 0U, 255U};
            }

            const int palette_size = std::min(png_palette_size, 256);
            for (int index = 0; index < palette_size; ++index)
            {
                colours[static_cast<std::size_t>(index)] = SDL_Color{
                    png_palette[index].red,
                    png_palette[index].green,
                    png_palette[index].blue,
                    255U
                };
            }

            png_bytep transparency = nullptr;
            int transparency_count = 0;
            png_color_16p transparent_colour = nullptr;
            if (png_get_tRNS(
                    png,
                    info,
                    &transparency,
                    &transparency_count,
                    &transparent_colour
                ) != 0 && transparency != nullptr)
            {
                const int alpha_count = std::min(transparency_count, palette_size);
                for (int index = 0; index < alpha_count; ++index)
                {
                    colours[static_cast<std::size_t>(index)].a = transparency[index];
                }
            }

            if (!SDL_SetPaletteColors(palette, colours.data(), 0, 256) ||
                !SDL_SetSurfacePalette(surface, palette))
            {
                png_error(png, SDL_GetError());
            }
            SDL_DestroyPalette(palette);
            palette = nullptr;
        }
        else
        {
            if (bit_depth == 16)
            {
                png_set_strip_16(png);
            }
            if (colour_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8)
            {
                png_set_expand_gray_1_2_4_to_8(png);
            }
            if (png_get_valid(png, info, PNG_INFO_tRNS) != 0)
            {
                png_set_tRNS_to_alpha(png);
            }
            if (colour_type == PNG_COLOR_TYPE_GRAY ||
                colour_type == PNG_COLOR_TYPE_GRAY_ALPHA)
            {
                png_set_gray_to_rgb(png);
            }
            if ((colour_type & PNG_COLOR_MASK_ALPHA) == 0 &&
                png_get_valid(png, info, PNG_INFO_tRNS) == 0)
            {
                png_set_add_alpha(png, 0xFFU, PNG_FILLER_AFTER);
            }

            (void)png_set_interlace_handling(png);
            png_read_update_info(png, info);

            surface = SDL_CreateSurface(
                static_cast<int>(png_width),
                static_cast<int>(png_height),
                SDL_PIXELFORMAT_RGBA32
            );
            if (surface == nullptr)
            {
                png_error(png, SDL_GetError());
            }

            const png_size_t row_bytes = png_get_rowbytes(png, info);
            if (row_bytes != static_cast<png_size_t>(png_width) * 4U)
            {
                png_error(png, "unsupported decoded PNG row format");
            }
        }

        if (surface->pitch < static_cast<int>(png_get_rowbytes(png, info)))
        {
            png_error(png, "SDL surface pitch is smaller than the PNG row size");
        }

        rows = static_cast<png_bytep*>(SDL_malloc(sizeof(png_bytep) * png_height));
        if (rows == nullptr)
        {
            png_error(png, "out of memory while creating PNG rows");
        }
        for (png_uint_32 y = 0U; y < png_height; ++y)
        {
            rows[y] = static_cast<png_bytep>(surface->pixels) +
                static_cast<std::size_t>(y) * static_cast<std::size_t>(surface->pitch);
        }

        png_read_image(png, rows);
        png_read_end(png, info);

        SDL_free(rows);
        rows = nullptr;
        png_destroy_read_struct(&png, &info, nullptr);
        SDL_ClearError();
        return surface;
    }

    bool ReadWholeStream(SDL_IOStream* stream, std::vector<Uint8>& output)
    {
        if (stream == nullptr)
        {
            SDL_SetError("The image stream is null.");
            return false;
        }

        const Sint64 signed_size = SDL_GetIOSize(stream);
        if (signed_size < 0 ||
            static_cast<Uint64>(signed_size) >
                static_cast<Uint64>(std::numeric_limits<std::size_t>::max()))
        {
            SDL_SetError("The image stream size is invalid.");
            return false;
        }

        output.resize(static_cast<std::size_t>(signed_size));
        if (!output.empty() && SDL_ReadIO(stream, output.data(), output.size()) != output.size())
        {
            SDL_SetError("The image stream could not be read completely.");
            return false;
        }
        return true;
    }

    bool SaveSurfaceAsPng(SDL_Surface* source, const char* filename)
    {
        if (source == nullptr || filename == nullptr || filename[0] == '\0')
        {
            return SDL_SetError("A valid surface and output filename are required.");
        }

        FILE* file = std::fopen(filename, "wb");
        if (file == nullptr)
        {
            return SDL_SetError("Could not open the PNG output file.");
        }

        PngErrorState error_state{};
        png_structp png = png_create_write_struct(
            PNG_LIBPNG_VER_STRING,
            &error_state,
            PngErrorHandler,
            PngWarningHandler
        );
        if (png == nullptr)
        {
            std::fclose(file);
            return SDL_SetError("libpng could not create its write context.");
        }

        png_infop info = png_create_info_struct(png);
        if (info == nullptr)
        {
            png_destroy_write_struct(&png, nullptr);
            std::fclose(file);
            return SDL_SetError("libpng could not create its output information context.");
        }

        SDL_Surface* converted = nullptr;
        SDL_Surface* writable = source;
        png_bytep* rows = nullptr;
        bool surface_locked = false;

        if (setjmp(png_jmpbuf(png)) != 0)
        {
            if (rows != nullptr)
            {
                SDL_free(rows);
            }
            if (surface_locked)
            {
                SDL_UnlockSurface(writable);
            }
            if (converted != nullptr)
            {
                SDL_DestroySurface(converted);
            }
            png_destroy_write_struct(&png, &info);
            std::fclose(file);
            return SDL_SetError(
                "libpng could not save the PNG: %s",
                error_state.message[0] != '\0'
                    ? error_state.message.data()
                    : "unknown error"
            );
        }

        png_init_io(png, file);

        const bool indexed = source->format == SDL_PIXELFORMAT_INDEX8 &&
            SDL_GetSurfacePalette(source) != nullptr;
        if (indexed)
        {
            SDL_Palette* source_palette = SDL_GetSurfacePalette(source);
            const int palette_size = std::clamp(source_palette->ncolors, 1, 256);
            std::array<png_color, 256> png_palette{};
            std::array<png_byte, 256> transparency{};
            int last_transparent = -1;

            for (int index = 0; index < palette_size; ++index)
            {
                const SDL_Color& colour = source_palette->colors[index];
                png_palette[static_cast<std::size_t>(index)] = png_color{
                    colour.r,
                    colour.g,
                    colour.b
                };
                transparency[static_cast<std::size_t>(index)] = colour.a;
                if (colour.a != 255U)
                {
                    last_transparent = index;
                }
            }

            png_set_IHDR(
                png,
                info,
                static_cast<png_uint_32>(source->w),
                static_cast<png_uint_32>(source->h),
                8,
                PNG_COLOR_TYPE_PALETTE,
                PNG_INTERLACE_NONE,
                PNG_COMPRESSION_TYPE_DEFAULT,
                PNG_FILTER_TYPE_DEFAULT
            );
            png_set_PLTE(png, info, png_palette.data(), palette_size);
            if (last_transparent >= 0)
            {
                png_set_tRNS(
                    png,
                    info,
                    transparency.data(),
                    last_transparent + 1,
                    nullptr
                );
            }
        }
        else
        {
            converted = SDL_ConvertSurface(source, SDL_PIXELFORMAT_RGBA32);
            if (converted == nullptr)
            {
                png_error(png, SDL_GetError());
            }
            writable = converted;
            png_set_IHDR(
                png,
                info,
                static_cast<png_uint_32>(writable->w),
                static_cast<png_uint_32>(writable->h),
                8,
                PNG_COLOR_TYPE_RGBA,
                PNG_INTERLACE_NONE,
                PNG_COMPRESSION_TYPE_DEFAULT,
                PNG_FILTER_TYPE_DEFAULT
            );
        }

        png_write_info(png, info);

        if (SDL_MUSTLOCK(writable))
        {
            if (!SDL_LockSurface(writable))
            {
                png_error(png, SDL_GetError());
            }
            surface_locked = true;
        }

        rows = static_cast<png_bytep*>(
            SDL_malloc(sizeof(png_bytep) * static_cast<std::size_t>(writable->h))
        );
        if (rows == nullptr)
        {
            png_error(png, "out of memory while creating PNG output rows");
        }
        for (int y = 0; y < writable->h; ++y)
        {
            rows[y] = static_cast<png_bytep>(writable->pixels) +
                static_cast<std::size_t>(y) * static_cast<std::size_t>(writable->pitch);
        }

        png_write_image(png, rows);
        png_write_end(png, info);

        SDL_free(rows);
        if (surface_locked)
        {
            SDL_UnlockSurface(writable);
        }
        if (converted != nullptr)
        {
            SDL_DestroySurface(converted);
        }
        png_destroy_write_struct(&png, &info);
        std::fclose(file);
        SDL_ClearError();
        return true;
    }
}

extern "C" SDL_Surface* SDLCALL IMG_Load(const char* file)
{
    if (file == nullptr || file[0] == '\0')
    {
        SDL_SetError("A valid PNG filename is required.");
        return nullptr;
    }

    SDL_IOStream* stream = SDL_IOFromFile(file, "rb");
    if (stream == nullptr)
    {
        return nullptr;
    }
    return IMG_LoadTyped_IO(stream, true, "PNG");
}

extern "C" SDL_Surface* SDLCALL IMG_LoadTyped_IO(
    SDL_IOStream* stream,
    const bool closeio,
    const char* type
)
{
    if (type != nullptr && type[0] != '\0' && SDL_strcasecmp(type, "PNG") != 0)
    {
        if (closeio && stream != nullptr)
        {
            SDL_CloseIO(stream);
        }
        SDL_SetError("The Web build currently accepts PNG images only.");
        return nullptr;
    }

    std::vector<Uint8> bytes;
    const bool read_success = ReadWholeStream(stream, bytes);
    if (closeio && stream != nullptr)
    {
        SDL_CloseIO(stream);
    }
    if (!read_success)
    {
        return nullptr;
    }
    return LoadPngFromMemory(bytes.data(), bytes.size());
}

extern "C" SDL_Texture* SDLCALL IMG_LoadTexture(SDL_Renderer* renderer, const char* file)
{
    if (renderer == nullptr)
    {
        SDL_SetError("A valid renderer is required.");
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

extern "C" bool SDLCALL IMG_SavePNG(SDL_Surface* surface, const char* file)
{
    return SaveSurfaceAsPng(surface, file);
}

#endif
