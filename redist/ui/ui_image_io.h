#pragma once

#include "types/sdl_handle_defs.h"

#include <filesystem>
#include <string>

namespace spintool
{
	[[nodiscard]] std::string PathToUtf8(const std::filesystem::path& path);

	[[nodiscard]] SDLSurfaceHandle LoadImageFromPath(
		const std::filesystem::path& path,
		std::string* error_message = nullptr
	);

	// Saves a PNG and, in WebAssembly builds, immediately downloads the file
	// through the browser after it has been written to the virtual filesystem.
	[[nodiscard]] bool SavePngToPath(
		SDL_Surface* surface,
		const std::filesystem::path& path,
		std::string* error_message = nullptr
	);
}
