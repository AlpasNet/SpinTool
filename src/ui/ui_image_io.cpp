#include "ui/ui_image_io.h"

#include "SDL3/SDL_error.h"
#include "SDL3/SDL_iostream.h"
#include "SDL3/SDL_image.h"

#include <cstdint>
#include <fstream>
#include <iterator>
#include <limits>
#include <vector>

namespace spintool
{
	std::string PathToUtf8(const std::filesystem::path& path)
	{
#if defined(__cpp_lib_char8_t)
		const std::u8string utf8_path = path.u8string();
		return std::string(
			reinterpret_cast<const char*>(utf8_path.data()),
			utf8_path.size()
		);
#else
		return path.u8string();
#endif
	}

	SDLSurfaceHandle LoadImageFromPath(
		const std::filesystem::path& path,
		std::string* error_message
	)
	{
		auto set_error = [error_message](const std::string& message)
		{
			if (error_message != nullptr)
			{
				*error_message = message;
			}
		};

		std::ifstream input(path, std::ios::binary);
		if (!input.is_open())
		{
			set_error("Could not open the selected image file.");
			return {};
		}

		input.seekg(0, std::ios::end);
		const std::streamoff file_size = input.tellg();
		if (file_size <= 0)
		{
			set_error("The selected image file is empty.");
			return {};
		}
		if (static_cast<std::uintmax_t>(file_size) >
			static_cast<std::uintmax_t>(std::numeric_limits<int>::max()))
		{
			set_error("The selected image file is too large.");
			return {};
		}

		input.seekg(0, std::ios::beg);
		std::vector<Uint8> file_bytes(static_cast<std::size_t>(file_size));
		if (!input.read(
			reinterpret_cast<char*>(file_bytes.data()),
			file_size
		))
		{
			set_error("Could not read the selected image file.");
			return {};
		}

		SDL_IOStream* stream = SDL_IOFromConstMem(
			file_bytes.data(),
			file_bytes.size()
		);
		if (stream == nullptr)
		{
			set_error(std::string("Could not create an image stream: ") + SDL_GetError());
			return {};
		}

		SDLSurfaceHandle surface{IMG_Load_IO(stream, true)};
		if (!surface)
		{
			set_error(std::string("SDL_image could not decode the image: ") + SDL_GetError());
			return {};
		}

		if (error_message != nullptr)
		{
			error_message->clear();
		}
		return surface;
	}
}
