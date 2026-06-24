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
}
