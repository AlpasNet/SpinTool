#include "ui/ui_sprite_importer.h"

#include "rom/spinball_rom.h"
#include "ui/ui_editor.h"
#include "ui/ui_palette_viewer.h"
#include "ui/ui_file_selector.h"
#include "ui/ui_image_io.h"
#include "rom/sprite.h"
#include "rom/compressed2_optimizer.h"
#include "rom/ssc_compressor.h"
#include "rom/ssc_decompressor.h"

#include "SDL3/SDL_image.h"
#include "imgui.h"

#include <algorithm>
#include <filesystem>
#include <iterator>
#include <iostream>
#include <limits>
#include <string>
#include <sstream>
#include <vector>
#include <utility>
#include "rom/tileset.h"

namespace
{
	constexpr float img_scale = 2.0f;


	const char* CompressionDisplayName(const spintool::CompressionAlgorithm algorithm)
	{
		switch (algorithm)
		{
			case spintool::CompressionAlgorithm::SSC:
				return "SSC";
			case spintool::CompressionAlgorithm::LZSS:
				return "Compressed2 / LZW";
			case spintool::CompressionAlgorithm::NONE:
			default:
				return "Uncompressed / unknown";
		}
	}

	void UpdateMegaDriveChecksum(spintool::rom::SpinballROM& rom)
	{
		if (rom.m_buffer.size() < 0x190U)
		{
			return;
		}

		Uint32 checksum = 0U;
		for (std::size_t offset = 0x200U; offset < rom.m_buffer.size(); offset += 2U)
		{
			Uint16 word = static_cast<Uint16>(rom.m_buffer[offset]) << 8U;
			if (offset + 1U < rom.m_buffer.size())
			{
				word = static_cast<Uint16>(word | rom.m_buffer[offset + 1U]);
			}
			checksum = (checksum + word) & 0xFFFFU;
		}
		rom.WriteUint16(0x18EU, static_cast<Uint16>(checksum));
	}

	struct TilesetWriteResult
	{
		bool success = false;
		bool changed = false;
		std::string message;
	};

	struct TilesetCompressionPlan
	{
		bool valid = false;
		bool identical = false;
		std::vector<Uint8> stream;
		std::size_t capacity = 0U;
		std::size_t baseline_size = 0U;
		std::string strategy;
		std::string message;
	};

	bool ReadIndexedTile(
		const SDL_Surface& surface,
		const int tile_x,
		const int tile_y,
		std::vector<Uint8>& pixels,
		std::vector<Uint8>& packed
	)
	{
		const int x_origin = tile_x * spintool::rom::TileSet::s_tile_width;
		const int y_origin = tile_y * spintool::rom::TileSet::s_tile_height;
		if (x_origin < 0 || y_origin < 0 ||
			x_origin + spintool::rom::TileSet::s_tile_width > surface.w ||
			y_origin + spintool::rom::TileSet::s_tile_height > surface.h ||
			surface.format != SDL_PIXELFORMAT_INDEX8)
		{
			return false;
		}

		pixels.clear();
		packed.clear();
		pixels.reserve(spintool::rom::TileSet::s_tile_total_pixels);
		packed.reserve(spintool::rom::TileSet::s_tile_total_bytes);
		const auto* source = static_cast<const Uint8*>(surface.pixels);
		for (int y = 0; y < spintool::rom::TileSet::s_tile_height; ++y)
		{
			const Uint8* row = source + ((y_origin + y) * surface.pitch) + x_origin;
			for (int x = 0; x < spintool::rom::TileSet::s_tile_width; x += 2)
			{
				const Uint8 left = static_cast<Uint8>(row[x] & 0x0FU);
				const Uint8 right = static_cast<Uint8>(row[x + 1] & 0x0FU);
				pixels.emplace_back(left);
				pixels.emplace_back(right);
				packed.emplace_back(static_cast<Uint8>((left << 4U) | right));
			}
		}
		return true;
	}

	TilesetCompressionPlan BuildTilesetCompressionPlan(
		spintool::EditorUI& owning_ui,
		const spintool::rom::TileSet& target,
		const spintool::rom::TileSet& edited
	)
	{
		TilesetCompressionPlan plan;
		const spintool::CompressionAlgorithm algorithm = target.compression_algorithm;
		if (algorithm == spintool::CompressionAlgorithm::NONE)
		{
			plan.message = "This tileset has no supported compression type.";
			return plan;
		}

		spintool::rom::SpinballROM& working_rom = owning_ui.GetROM();
		const Uint32 offset = target.rom_data.rom_offset;
		spintool::TilesetEntry current_entry =
			spintool::rom::TileSet::LoadFromROM(working_rom, offset, algorithm);
		if (!current_entry.tileset || current_entry.result.error_msg.has_value())
		{
			plan.message = "The current compressed tileset could not be decoded.";
			return plan;
		}

		if (current_entry.tileset->num_tiles == edited.num_tiles &&
			current_entry.tileset->uncompressed_data == edited.uncompressed_data)
		{
			plan.valid = true;
			plan.identical = true;
			plan.capacity = current_entry.tileset->rom_data.real_size;
			plan.message = "The selected PNG is identical to the current ROM data.";
			return plan;
		}

		plan.capacity = current_entry.tileset->rom_data.real_size;
		spintool::rom::SpinballROM reference_rom;
		const std::filesystem::path reference_path = owning_ui.GetReferenceROMPath();
		if (!reference_path.empty() && reference_rom.LoadROMFromPath(reference_path))
		{
			spintool::TilesetEntry reference_entry =
				spintool::rom::TileSet::LoadFromROM(reference_rom, offset, algorithm);
			if (reference_entry.tileset && !reference_entry.result.error_msg.has_value())
			{
				plan.capacity = reference_entry.tileset->rom_data.real_size;
			}
		}

		if (offset > working_rom.m_buffer.size() ||
			plan.capacity > working_rom.m_buffer.size() - offset)
		{
			plan.message = "The compressed tileset block is outside the working ROM.";
			return plan;
		}

		if (algorithm == spintool::CompressionAlgorithm::SSC)
		{
			const spintool::rom::SSCCompressionResult compressed =
				spintool::rom::SSCCompressor::CompressData(
					edited.uncompressed_data,
					0U,
					static_cast<Uint32>(edited.uncompressed_data.size())
				);
			plan.stream.reserve(2U + compressed.size());
			plan.stream.emplace_back(static_cast<Uint8>((edited.num_tiles >> 8U) & 0xFFU));
			plan.stream.emplace_back(static_cast<Uint8>(edited.num_tiles & 0xFFU));
			plan.stream.insert(plan.stream.end(), compressed.begin(), compressed.end());
			const spintool::rom::SSCDecompressionResult verification =
				spintool::rom::SSCDecompressor::DecompressData(
					plan.stream,
					2U,
					static_cast<Uint32>(edited.uncompressed_data.size())
				);
			if (verification.error_msg.has_value() ||
				verification.uncompressed_data != edited.uncompressed_data)
			{
				plan.message = "SSC compression verification produced different tile data.";
				return plan;
			}
			plan.strategy = "optimized SSC";
			plan.baseline_size = plan.stream.size();
		}
		else
		{
			const std::size_t current_size = current_entry.tileset->rom_data.real_size;
			if (current_size > working_rom.m_buffer.size() - offset)
			{
				plan.message = "The current Compressed2 stream is invalid.";
				return plan;
			}
			const std::vector<Uint8> original_stream(
				working_rom.m_buffer.begin() + static_cast<std::ptrdiff_t>(offset),
				working_rom.m_buffer.begin() + static_cast<std::ptrdiff_t>(offset + current_size)
			);
			const spintool::rom::Compressed2CompressionResult compression =
				spintool::rom::Compressed2Optimizer::Compress(
					edited.uncompressed_data,
					original_stream
				);
			std::vector<Uint8> verified;
			std::string error;
			std::size_t consumed = 0U;
			if (!spintool::rom::Compressed2Optimizer::Decode(
				compression.data,
				0U,
				verified,
				error,
				&consumed,
				nullptr
			) || verified != edited.uncompressed_data || consumed != compression.data.size())
			{
				plan.message = "Compressed2/LZW verification failed.";
				return plan;
			}
			plan.stream = compression.data;
			plan.strategy = compression.strategy;
			plan.baseline_size = compression.baseline_size;
		}

		if (plan.stream.size() > plan.capacity)
		{
			std::ostringstream message;
			message << "The edited tileset needs " << plan.stream.size()
				<< " bytes after " << CompressionDisplayName(algorithm)
				<< " compression, but the original block only has " << plan.capacity
				<< " bytes (" << plan.stream.size() - plan.capacity << " bytes too large).";
			plan.message = message.str();
			return plan;
		}

		plan.valid = true;
		plan.message = "The PNG, tile layout and compressed data were verified successfully.";
		return plan;
	}

	TilesetWriteResult WriteTilesetToROM(
		spintool::EditorUI& owning_ui,
		spintool::rom::TileSet& target,
		const spintool::rom::TileSet& edited,
		const bool full_tileset_import
	)
	{
		TilesetWriteResult result;
		const TilesetCompressionPlan plan = BuildTilesetCompressionPlan(
			owning_ui,
			target,
			edited
		);
		if (!plan.valid)
		{
			result.message = "Import refused: " + plan.message;
			return result;
		}
		if (plan.identical)
		{
			result.success = true;
			result.changed = false;
			result.message = full_tileset_import
				? "The PNG is identical to the current tileset; the ROM was not modified."
				: "The PNG is identical to the current tile; the ROM was not modified.";
			return result;
		}

		spintool::rom::SpinballROM& working_rom = owning_ui.GetROM();
		const Uint32 offset = target.rom_data.rom_offset;
		const std::vector<Uint8> previous_block(
			working_rom.m_buffer.begin() + static_cast<std::ptrdiff_t>(offset),
			working_rom.m_buffer.begin() + static_cast<std::ptrdiff_t>(offset + plan.capacity)
		);
		const Uint16 previous_checksum = working_rom.ReadUint16(0x18EU);
		std::copy(plan.stream.begin(), plan.stream.end(), working_rom.m_buffer.begin() + offset);
		std::fill(
			working_rom.m_buffer.begin() + static_cast<std::ptrdiff_t>(offset + plan.stream.size()),
			working_rom.m_buffer.begin() + static_cast<std::ptrdiff_t>(offset + plan.capacity),
			0U
		);
		UpdateMegaDriveChecksum(working_rom);
		const std::filesystem::path working_path = owning_ui.GetWorkingROMPath();
		if (working_path.empty())
		{
			std::copy(previous_block.begin(), previous_block.end(), working_rom.m_buffer.begin() + offset);
			working_rom.WriteUint16(0x18EU, previous_checksum);
			result.message = "Import refused: no working ROM path is available.";
			return result;
		}
		working_rom.m_filepath = working_path;
		working_rom.SaveROM();

		spintool::rom::SpinballROM verified_rom;
		spintool::TilesetEntry verified_entry;
		if (verified_rom.LoadROMFromPath(working_path))
		{
			verified_entry = spintool::rom::TileSet::LoadFromROM(
				verified_rom,
				offset,
				target.compression_algorithm
			);
		}
		if (!verified_entry.tileset || verified_entry.result.error_msg.has_value() ||
			verified_entry.tileset->num_tiles != edited.num_tiles ||
			verified_entry.tileset->uncompressed_data != edited.uncompressed_data)
		{
			std::copy(previous_block.begin(), previous_block.end(), working_rom.m_buffer.begin() + offset);
			working_rom.WriteUint16(0x18EU, previous_checksum);
			working_rom.SaveROM();
			result.message = "The tileset write could not be verified; the previous ROM data was restored.";
			return result;
		}

		const Uint64 next_revision = target.revision + 1U;
		target = std::move(*verified_entry.tileset);
		target.revision = next_revision;
		result.success = true;
		result.changed = true;
		std::ostringstream message;
		message << (full_tileset_import
			? "Full tileset imported and verified in rom_export. "
			: "Tile imported and verified in rom_export. ")
			<< CompressionDisplayName(target.compression_algorithm) << " size "
			<< plan.stream.size() << "/" << plan.capacity << " bytes; "
			<< plan.capacity - plan.stream.size() << " bytes remaining";
		if (!plan.strategy.empty())
		{
			message << "; strategy " << plan.strategy;
		}
		if (plan.baseline_size > plan.stream.size())
		{
			message << "; saved " << plan.baseline_size - plan.stream.size()
				<< " bytes versus basic compression";
		}
		message << ".";
		result.message = message.str();
		return result;
	}

	std::vector<Uint8> ConvertTileSurfaceToIndexed(
		SDL_Surface* source,
		const std::vector<std::shared_ptr<spintool::rom::Palette>>& palettes
	)
	{
		if (source == nullptr || source->w <= 0 || source->h <= 0 || source->pixels == nullptr)
		{
			return {};
		}

		const std::size_t pixel_count =
			static_cast<std::size_t>(source->w) * static_cast<std::size_t>(source->h);
		std::vector<Uint8> output(pixel_count, 0U);

		// SpinTool exports indexed PNG files. Preserve their tile colour indices
		// directly: tile graphics are palette-independent and only store 0-15.
		if (source->format == SDL_PIXELFORMAT_INDEX8)
		{
			for (int y = 0; y < source->h; ++y)
			{
				const Uint8* row = static_cast<const Uint8*>(source->pixels) +
					static_cast<std::size_t>(y) * source->pitch;
				for (int x = 0; x < source->w; ++x)
				{
					output[static_cast<std::size_t>(y) * source->w + x] =
						static_cast<Uint8>(row[x] & 0x0FU);
				}
			}
			return output;
		}

		// RGB/RGBA PNG files are accepted without showing a palette-selection
		// screen. Pick the loaded level palette line that best matches the image,
		// then convert every pixel automatically to its nearest 0-15 index.
		SDLSurfaceHandle converted{ SDL_ConvertSurface(source, SDL_PIXELFORMAT_RGBA32) };
		if (!converted)
		{
			return {};
		}

		std::vector<const spintool::rom::Palette*> candidates;
		for (const auto& palette : palettes)
		{
			if (palette)
			{
				candidates.emplace_back(palette.get());
			}
		}
		if (candidates.empty())
		{
			return {};
		}

		const SDL_PixelFormatDetails* format = SDL_GetPixelFormatDetails(converted->format);
		if (format == nullptr)
		{
			return {};
		}
		const auto extract_channel = [](const Uint32 packed, const Uint32 mask, const Uint8 shift) -> Uint8
		{
			return mask == 0U ? 0xFFU : static_cast<Uint8>((packed & mask) >> shift);
		};

		const spintool::rom::Palette* best_palette = candidates.front();
		Uint64 best_total = std::numeric_limits<Uint64>::max();
		for (const spintool::rom::Palette* candidate : candidates)
		{
			Uint64 total = 0U;
			for (int y = 0; y < converted->h; ++y)
			{
				const Uint8* row = static_cast<const Uint8*>(converted->pixels) +
					static_cast<std::size_t>(y) * converted->pitch;
				for (int x = 0; x < converted->w; ++x)
				{
					const Uint32 packed = reinterpret_cast<const Uint32*>(row)[x];
					const Uint8 alpha = format->Amask == 0U
						? 0xFFU
						: extract_channel(packed, format->Amask, format->Ashift);
					if (alpha < 0x80U)
					{
						continue;
					}
					const Uint8 red = extract_channel(packed, format->Rmask, format->Rshift);
					const Uint8 green = extract_channel(packed, format->Gmask, format->Gshift);
					const Uint8 blue = extract_channel(packed, format->Bmask, format->Bshift);
					Uint32 nearest = std::numeric_limits<Uint32>::max();
					for (Uint8 index = 0U; index < 16U; ++index)
					{
						const spintool::rom::Colour colour =
							candidate->palette_swatches[index].GetUnpacked();
						const int dr = static_cast<int>(red) - colour.r;
						const int dg = static_cast<int>(green) - colour.g;
						const int db = static_cast<int>(blue) - colour.b;
						nearest = std::min(nearest, static_cast<Uint32>(dr * dr + dg * dg + db * db));
					}
					total += nearest;
				}
			}
			if (total < best_total)
			{
				best_total = total;
				best_palette = candidate;
			}
		}

		for (int y = 0; y < converted->h; ++y)
		{
			const Uint8* row = static_cast<const Uint8*>(converted->pixels) +
				static_cast<std::size_t>(y) * converted->pitch;
			for (int x = 0; x < converted->w; ++x)
			{
				const Uint32 packed = reinterpret_cast<const Uint32*>(row)[x];
				const Uint8 alpha = format->Amask == 0U
					? 0xFFU
					: extract_channel(packed, format->Amask, format->Ashift);
				const std::size_t pixel_index = static_cast<std::size_t>(y) * converted->w + x;
				if (alpha < 0x80U)
				{
					output[pixel_index] = 0U;
					continue;
				}
				const Uint8 red = extract_channel(packed, format->Rmask, format->Rshift);
				const Uint8 green = extract_channel(packed, format->Gmask, format->Gshift);
				const Uint8 blue = extract_channel(packed, format->Bmask, format->Bshift);
				Uint8 best_index = 0U;
				Uint32 best_distance = std::numeric_limits<Uint32>::max();
				for (Uint8 index = 0U; index < 16U; ++index)
				{
					const spintool::rom::Colour colour =
						best_palette->palette_swatches[index].GetUnpacked();
					const int dr = static_cast<int>(red) - colour.r;
					const int dg = static_cast<int>(green) - colour.g;
					const int db = static_cast<int>(blue) - colour.b;
					const Uint32 distance = static_cast<Uint32>(dr * dr + dg * dg + db * db);
					if (distance < best_distance)
					{
						best_distance = distance;
						best_index = index;
					}
				}
				output[pixel_index] = best_index;
			}
		}
		return output;
	}

	TilesetWriteResult ImportTilesetImageImmediately(
		spintool::EditorUI& owning_ui,
		spintool::rom::TileSet& target,
		const std::optional<std::size_t> target_tile_index,
		SDL_Surface* source,
		const std::vector<std::shared_ptr<spintool::rom::Palette>>& palettes
	)
	{
		TilesetWriteResult result;
		if (source == nullptr)
		{
			result.message = "Import failed: the selected PNG could not be decoded.";
			return result;
		}

		const bool single_tile = target_tile_index.has_value();
		const int expected_tiles_wide = std::min<int>(
			20,
			static_cast<int>(target.num_tiles)
		);
		const int expected_tiles_high = target.num_tiles == 0U
			? 0
			: (static_cast<int>(target.num_tiles) + 19) / 20;
		const int expected_width = single_tile
			? spintool::rom::TileSet::s_tile_width
			: expected_tiles_wide * spintool::rom::TileSet::s_tile_width;
		const int expected_height = single_tile
			? spintool::rom::TileSet::s_tile_height
			: expected_tiles_high * spintool::rom::TileSet::s_tile_height;
		if (source->w != expected_width || source->h != expected_height)
		{
			std::ostringstream message;
			message << "Import failed: expected a " << expected_width << " x "
				<< expected_height << " PNG, but the selected image is "
				<< source->w << " x " << source->h << ".";
			result.message = message.str();
			return result;
		}

		const std::vector<Uint8> indexed = ConvertTileSurfaceToIndexed(source, palettes);
		if (indexed.size() != static_cast<std::size_t>(source->w) * source->h)
		{
			result.message = "Import failed: the PNG pixels could not be converted to tile indices.";
			return result;
		}

		spintool::TilesetEntry editable_entry = spintool::rom::TileSet::LoadFromROM(
			owning_ui.GetROM(),
			target.rom_data.rom_offset,
			target.compression_algorithm
		);
		auto edited = std::move(editable_entry.tileset);
		if (!edited || editable_entry.result.error_msg.has_value())
		{
			result.message = "Import failed: the current tileset could not be decoded.";
			return result;
		}

		auto write_tile = [&](const std::size_t tile_index, const int tile_x, const int tile_y) -> bool
		{
			if (tile_index >= edited->tiles.size())
			{
				return false;
			}
			const std::size_t byte_offset =
				tile_index * spintool::rom::TileSet::s_tile_total_bytes;
			if (byte_offset > edited->uncompressed_data.size() ||
				spintool::rom::TileSet::s_tile_total_bytes >
					edited->uncompressed_data.size() - byte_offset)
			{
				return false;
			}

			auto& tile_pixels = edited->tiles[tile_index].pixel_data;
			tile_pixels.assign(spintool::rom::TileSet::s_tile_total_pixels, 0U);
			std::size_t packed_offset = byte_offset;
			for (int y = 0; y < spintool::rom::TileSet::s_tile_height; ++y)
			{
				for (int x = 0; x < spintool::rom::TileSet::s_tile_width; x += 2)
				{
					const int source_x = tile_x * spintool::rom::TileSet::s_tile_width + x;
					const int source_y = tile_y * spintool::rom::TileSet::s_tile_height + y;
					const std::size_t source_index =
						static_cast<std::size_t>(source_y) * source->w + source_x;
					const Uint8 left = static_cast<Uint8>(indexed[source_index] & 0x0FU);
					const Uint8 right = static_cast<Uint8>(indexed[source_index + 1U] & 0x0FU);
					const std::size_t pixel_offset =
						static_cast<std::size_t>(y) * spintool::rom::TileSet::s_tile_width + x;
					tile_pixels[pixel_offset] = left;
					tile_pixels[pixel_offset + 1U] = right;
					edited->uncompressed_data[packed_offset++] =
						static_cast<Uint8>((left << 4U) | right);
				}
			}
			return true;
		};

		if (single_tile)
		{
			if (!write_tile(*target_tile_index, 0, 0))
			{
				result.message = "Import failed: the selected tile index is invalid.";
				return result;
			}
		}
		else
		{
			for (std::size_t tile_index = 0; tile_index < edited->num_tiles; ++tile_index)
			{
				if (!write_tile(
					tile_index,
					static_cast<int>(tile_index % 20U),
					static_cast<int>(tile_index / 20U)
				))
				{
					result.message = "Import failed: the full tileset data is inconsistent.";
					return result;
				}
			}
		}

		edited->uncompressed_size = static_cast<Uint32>(edited->uncompressed_data.size());
		return WriteTilesetToROM(owning_ui, target, *edited, !single_tile);
	}

}

namespace spintool
{
	void EditorImageImporter::InnerUpdate()
	{
		static char path_buffer[4096] = "";
		bool update_preview = false;
		const bool tileset_mode = std::holds_alternative<rom::TileSet*>(m_target_asset);
		const bool single_tile_mode = tileset_mode && m_target_tile_index.has_value();

		static FileSelectorSettings settings;
		settings.object_typename = tileset_mode
			? (single_tile_mode ? "Tile PNG" : "Full Tileset PNG")
			: "Image";
		settings.target_directory = m_owning_ui.GetSpriteExportPath();
		settings.file_extension_filter = tileset_mode
			? std::vector<std::string>{ ".png" }
			: std::vector<std::string>{ ".png", ".gif", ".bmp" };
		settings.tiled_previews = true;
		settings.num_columns = 4;
		settings.open_popup = std::exchange(m_open_image_selector, false);
		settings.close_popup = std::exchange(m_close_image_selector, false);

		std::optional<std::filesystem::path> current_selection;
		if (!m_loaded_path.empty())
		{
			current_selection = std::filesystem::path{m_loaded_path};
		}
		const std::optional<std::filesystem::path> selected_path = DrawFileSelector(
			settings,
			m_owning_ui,
			current_selection
		);

		if (tileset_mode)
		{
			ImGui::TextUnformatted(single_tile_mode
				? "Choose an 8 x 8 PNG. The selected tile is imported immediately."
				: "Choose a full tileset PNG. It is imported immediately.");

			if (selected_path)
			{
				m_close_image_selector = true;
				std::string load_error;
				SDLSurfaceHandle loaded_surface = LoadImageFromPath(*selected_path, &load_error);
				if (!loaded_surface)
				{
					m_import_status = load_error.empty()
						? "Import failed: the selected PNG could not be loaded."
						: load_error;
					m_tile_import_result = m_import_status;
					std::cerr << m_tile_import_result << '\n';
				}
				else
				{
					rom::TileSet& target_tileset = *std::get<rom::TileSet*>(m_target_asset);
					const TilesetWriteResult import_result = ImportTilesetImageImmediately(
						m_owning_ui,
						target_tileset,
						m_target_tile_index,
						loaded_surface.get(),
						m_available_palettes
					);
					m_import_status = import_result.message;
					m_tile_import_result = m_import_status;
					if (import_result.success)
					{
						std::cout << m_tile_import_result << '\n';
					}
					else
					{
						std::cerr << m_tile_import_result << '\n';
					}
				}

				// The file has already been processed. Return to the editor and show
				// a text result popup, regardless of success or failure.
				m_visible = false;
				return;
			}

			if (!m_import_status.empty())
			{
				ImGui::TextWrapped("%s", m_import_status.c_str());
			}
			if (ImGui::Button("Choose PNG"))
			{
				m_open_image_selector = true;
			}
			return;
		}

		ImGui::InputText("Path", path_buffer, sizeof(path_buffer));
		ImGui::SameLine();
		if (ImGui::Button("Choose Image"))
		{
			m_open_image_selector = true;
		}

		if (m_available_palettes.empty())
		{
			return;
		}

		if (selected_path)
		{
			m_close_image_selector = true;
			const std::string selected_path_utf8 = PathToUtf8(*selected_path);
			std::string load_error;
			SDLSurfaceHandle loaded_surface = tileset_mode
				? LoadImageFromPath(*selected_path, &load_error)
				: SDLSurfaceHandle{IMG_Load(selected_path_utf8.c_str())};
			if (loaded_surface != nullptr)
			{
				m_loaded_path = selected_path_utf8;
				m_imported_image = SDLSurfaceHandle{ SDL_ConvertSurface(loaded_surface.get(), SDL_PIXELFORMAT_RGBA32) };
				m_rendered_imported_image = Renderer::RenderToTexture(m_imported_image.get());
				m_detected_colours.clear();
				if (tileset_mode)
				{
					m_result_asset = std::unique_ptr<rom::TileSet>{};
				}
				m_force_update_write_location = true;
				m_import_status.clear();
				m_tileset_validation_ready = false;
			}
			else
			{
				m_import_status = load_error.empty()
					? "The selected image could not be loaded."
					: load_error;
			}
		}

		if (DrawPaletteSelector(m_selected_palette_index, m_available_palettes))
		{
			update_preview = true;
		}
		m_selected_palette_index = std::clamp(m_selected_palette_index, 0, static_cast<int>(m_available_palettes.size()) - 1);

		ImGui::SameLine();
		DrawPaletteName(*m_available_palettes.at(m_selected_palette_index), m_selected_palette_index);
		DrawPaletteSwatchPreview(*m_available_palettes.at(m_selected_palette_index));
		m_selected_palette = *m_available_palettes.at(m_selected_palette_index);

		if (m_imported_image == nullptr)
		{
			if (!m_import_status.empty())
			{
				ImGui::TextWrapped("%s", m_import_status.c_str());
			}
			return;
		}
		ImGui::Text("Loaded: %s", m_loaded_path.c_str());

		if (m_rendered_imported_image == nullptr)
		{
			ImGui::Text("Rendering...");
			return;
		}

		ImGui::BeginGroup();
		{
			ImGui::Image((ImTextureID)m_rendered_imported_image.get(), { static_cast<float>(m_imported_image->w) * 2, static_cast<float>(m_imported_image->h) * 2 });

			if (ImGui::Button("Attempt to match colours"))
			{
				m_detected_colours.clear();
			}

			if (ImGui::Button("Force Update Preview"))
			{
				update_preview = true;
			}

			ImGui::Text("Palette colour mapping");
			ImGui::BeginDisabled();
			ImGui::Text("Original Colour -> Palette Colour Index");
			ImGui::EndDisabled();
			if (m_detected_colours.empty())
			{
				const SDL_PixelFormatDetails* pixel_format_details = SDL_GetPixelFormatDetails(m_imported_image->format);
				for (size_t i = 0; i < m_imported_image->w * m_imported_image->h; ++i)
				{
					const size_t byte_offset = pixel_format_details->bytes_per_pixel * i;
					const Uint32 packed_pixel = *reinterpret_cast<Uint32*>(&static_cast<Uint8*>(m_imported_image->pixels)[byte_offset]);
					const ImColor pixel
					{ static_cast<int>((packed_pixel & pixel_format_details->Rmask) >> pixel_format_details->Rshift)
					, static_cast<int>((packed_pixel & pixel_format_details->Gmask) >> pixel_format_details->Gshift)
					, static_cast<int>((packed_pixel & pixel_format_details->Bmask) >> pixel_format_details->Bshift)
					, static_cast<int>((packed_pixel & pixel_format_details->Amask) >> pixel_format_details->Ashift) };

					if (std::none_of(std::begin(m_detected_colours), std::end(m_detected_colours),
						[&pixel](const ColourPaletteMapping& colour_entry)
						{
							if (pixel.Value.w == 0)
							{
								return colour_entry.colour.Value.w == pixel.Value.w;;
							}

							return colour_entry.colour.Value.x == pixel.Value.x
								&& colour_entry.colour.Value.y == pixel.Value.y
								&& colour_entry.colour.Value.z == pixel.Value.z
								&& colour_entry.colour.Value.w == pixel.Value.w;
						}))
					{
						const auto& swatches = m_selected_palette.palette_swatches;
						const Uint16 packed_pixel_value = rom::Swatch::Pack(pixel.Value.x, pixel.Value.y, pixel.Value.z);

						const auto matched_colour = std::find_if(std::begin(swatches), std::end(swatches),
							[&packed_pixel_value](const rom::Swatch& swatch)
							{
								return swatch.packed_value == packed_pixel_value;
							});

						const Uint8 mapped_palette_index = (matched_colour != std::end(swatches)) ? static_cast<Uint8>(std::distance(std::begin(swatches), matched_colour)) : 0;
						m_detected_colours.emplace_back(ColourPaletteMapping{ pixel, mapped_palette_index });
					}
				}

				if (m_detected_colours.empty())
				{
					ImGui::EndGroup();
					return;
				}

				update_preview = true;
			}
			int i = 0;
			for (ColourPaletteMapping& colour_entry : m_detected_colours)
			{
				char id_buffer[64];
				sprintf(id_buffer, "col_%d", i);
				ImGui::ColorButton(id_buffer, colour_entry.colour);
				ImGui::SameLine();
				ImGui::Text("->");
				ImGui::SameLine();
				const rom::Swatch& swatch = m_selected_palette.palette_swatches[colour_entry.palette_index];
				ImColor col = swatch.AsImColor();
				ImGui::ColorButton(id_buffer, col.Value);

				sprintf(id_buffer, "###pal_sel_%d", i);
				int out_val = colour_entry.palette_index;
				ImGui::SameLine();
				ImGui::SetNextItemWidth(64);
				if (ImGui::InputInt(id_buffer, &out_val))
				{
					colour_entry.palette_index = static_cast<Uint8>(out_val) % 16;
					update_preview = true;
				}
				if (colour_entry.colour.Value.w == 0)
				{
					ImGui::SameLine();
					ImGui::Text("(Transparent)");
				}
				++i;
			}
		}
		ImGui::EndGroup();

		ImGui::SameLine();

		ImGui::BeginGroup();
		{
			if (update_preview)
			{
				m_preview_image = SDLSurfaceHandle{ SDL_CreateSurface(m_imported_image->w, m_imported_image->h, SDL_PIXELFORMAT_INDEX8) };
				m_preview_palette = Renderer::CreateSDLPalette(m_selected_palette);
				SDL_SetSurfacePalette(m_preview_image.get(), m_preview_palette.get());
				const SDL_PixelFormatDetails* import_pixel_format_details = SDL_GetPixelFormatDetails(m_imported_image->format);
				const SDL_PixelFormatDetails* preview_pixel_format_details = SDL_GetPixelFormatDetails(m_preview_image->format);

				const size_t preview_pitch_offset_per_line = m_preview_image->pitch - (m_preview_image->w * preview_pixel_format_details->bytes_per_pixel);
				size_t preview_pitch_offset = 0;

				for (size_t i = 0; i < m_preview_image->w * m_preview_image->h; ++i)
				{
					if (i != 0 && i % m_preview_image->w == 0)
					{
						preview_pitch_offset += preview_pitch_offset_per_line;
					}

					const size_t import_byte_offset = import_pixel_format_details->bytes_per_pixel * i;
					const Uint32 packed_pixel = static_cast<Uint32*>(m_imported_image->pixels)[i];
					const ImColor pixel
					{ static_cast<int>((packed_pixel & import_pixel_format_details->Rmask) >> import_pixel_format_details->Rshift)
					, static_cast<int>((packed_pixel & import_pixel_format_details->Gmask) >> import_pixel_format_details->Gshift)
					, static_cast<int>((packed_pixel & import_pixel_format_details->Bmask) >> import_pixel_format_details->Bshift)
					, static_cast<int>((packed_pixel & import_pixel_format_details->Amask) >> import_pixel_format_details->Ashift) };

					auto result_entry = std::find_if(std::begin(m_detected_colours), std::end(m_detected_colours),
						[&pixel](const ColourPaletteMapping& colour_entry)
						{
							if (pixel.Value.w == 0)
							{
								return colour_entry.colour.Value.w == pixel.Value.w;;
							}
							return colour_entry.colour.Value.x == pixel.Value.x
								&& colour_entry.colour.Value.y == pixel.Value.y
								&& colour_entry.colour.Value.z == pixel.Value.z
								&& colour_entry.colour.Value.w == pixel.Value.w;
						});

					if (result_entry != std::end(m_detected_colours))
					{
						static_cast<Uint8*>(m_preview_image->pixels)[i + preview_pitch_offset] = result_entry->palette_index;
					}
					else
					{
						static_cast<Uint8*>(m_preview_image->pixels)[i + preview_pitch_offset] = 0;
					}

				}

				m_rendered_preview_image = Renderer::RenderToTexture(m_preview_image.get());
				m_force_update_write_location = true;
				if (tileset_mode)
				{
					m_tileset_validation_ready = false;
				}
			}
			else
			{
				if (m_rendered_preview_image == nullptr)
				{
					ImGui::Text("Rendering...");
					ImGui::EndGroup();
					return;
				}
			}

			if (m_rendered_preview_image != nullptr)
			{
				ImGui::Image((ImTextureID)m_rendered_preview_image.get(), { static_cast<float>(m_preview_image->w) * img_scale, static_cast<float>(m_preview_image->h) * img_scale });

				if (std::holds_alternative<rom::Sprite*>(m_target_asset))
				{
					DrawSpriteImport();
				}
				else if (std::holds_alternative<rom::TileSet*>(m_target_asset))
				{
					DrawTileSetImport();
				}
			}
		}
		ImGui::EndGroup();
	}

	static constexpr Uint32 picker_width = 20;

	void EditorImageImporter::RenderTileset(rom::TileSet& tileset)
	{
		m_export_preview_image = tileset.RenderToSurface(m_selected_palette);
		m_export_preview_texture = Renderer::RenderToTexture(m_export_preview_image.get());
	}

	void EditorImageImporter::DrawTileSetImport()
	{
		rom::TileSet& target_tileset = *std::get<rom::TileSet*>(m_target_asset);
		ImGui::Text("Compression: %s", CompressionDisplayName(target_tileset.compression_algorithm));

		if (!m_import_status.empty())
		{
			ImGui::TextWrapped("%s", m_import_status.c_str());
		}

		const bool single_tile_mode = m_target_tile_index.has_value();
		if (single_tile_mode)
		{
			ImGui::Text("Target tile: %zu", *m_target_tile_index);
			if (m_preview_image->w != rom::TileSet::s_tile_width ||
				m_preview_image->h != rom::TileSet::s_tile_height)
			{
				m_tileset_validation_ready = true;
				m_tileset_validation_valid = false;
				m_tileset_validation_identical = false;
				m_tileset_validation_message = "Invalid PNG dimensions: a tile import requires exactly 8 x 8 pixels.";
				ImGui::TextDisabled("Tile imports require an exact 8 x 8 image.");
				ImGui::SeparatorText("Import validation");
				ImGui::TextWrapped("Validation failed: %s", m_tileset_validation_message.c_str());
				return;
			}
		}
		else
		{
			const int expected_tiles_wide = std::min<int>(
				picker_width,
				static_cast<int>(target_tileset.num_tiles)
			);
			const int expected_tiles_high =
				(static_cast<int>(target_tileset.num_tiles) + picker_width - 1) /
				picker_width;
			const int expected_width = expected_tiles_wide * rom::TileSet::s_tile_width;
			const int expected_height = expected_tiles_high * rom::TileSet::s_tile_height;

			ImGui::Text(
				"Import mode: Full tileset (%u tiles)",
				static_cast<unsigned int>(target_tileset.num_tiles)
			);
			ImGui::Text("Expected PNG size: %d x %d pixels", expected_width, expected_height);
			ImGui::TextDisabled(
				"Use the PNG exported by 'Export full tileset as PNG' and keep the same tile layout."
			);

			if (target_tileset.num_tiles == 0U ||
				m_preview_image->w != expected_width ||
				m_preview_image->h != expected_height)
			{
				m_tileset_validation_ready = true;
				m_tileset_validation_valid = false;
				m_tileset_validation_identical = false;
				std::ostringstream validation_message;
				validation_message << "Invalid PNG dimensions: this tileset requires exactly "
					<< expected_width << " x " << expected_height << " pixels.";
				m_tileset_validation_message = validation_message.str();
				ImGui::TextDisabled(
					"Full tileset imports require the exact exported dimensions: %d x %d pixels.",
					expected_width,
					expected_height
				);
				ImGui::SeparatorText("Import validation");
				ImGui::TextWrapped("Validation failed: %s", m_tileset_validation_message.c_str());
				return;
			}
		}

		rom::TileSet* result_tileset = nullptr;
		if (auto* result = std::get_if<std::unique_ptr<rom::TileSet>>(&m_result_asset))
		{
			result_tileset = result->get();
		}

		if (m_force_update_write_location || result_tileset == nullptr)
		{
			m_force_update_write_location = false;
			TilesetEntry editable_entry = rom::TileSet::LoadFromROM(
				m_owning_ui.GetROM(),
				target_tileset.rom_data.rom_offset,
				target_tileset.compression_algorithm
			);
			auto edited = std::move(editable_entry.tileset);
			if (!edited || editable_entry.result.error_msg.has_value())
			{
				m_import_status = "The current tileset could not be decoded for editing.";
				m_result_asset = std::unique_ptr<rom::TileSet>{};
				return;
			}
			std::vector<Uint8> pixels;
			std::vector<Uint8> packed;

			if (single_tile_mode)
			{
				const std::size_t tile_index = *m_target_tile_index;
				if (tile_index >= edited->tiles.size() ||
					tile_index * rom::TileSet::s_tile_total_bytes > edited->uncompressed_data.size() ||
					rom::TileSet::s_tile_total_bytes >
						edited->uncompressed_data.size() - tile_index * rom::TileSet::s_tile_total_bytes ||
					!ReadIndexedTile(*m_preview_image, 0, 0, pixels, packed))
				{
					m_import_status = "The selected tile could not be prepared for import.";
					m_result_asset = std::unique_ptr<rom::TileSet>{};
					return;
				}
				edited->tiles[tile_index].pixel_data = pixels;
				const std::size_t byte_offset = tile_index * rom::TileSet::s_tile_total_bytes;
				std::copy(
					packed.begin(),
					packed.end(),
					edited->uncompressed_data.begin() + static_cast<std::ptrdiff_t>(byte_offset)
				);
			}
			else
			{
				const std::size_t required_data_size =
					static_cast<std::size_t>(edited->num_tiles) *
					rom::TileSet::s_tile_total_bytes;
				if (edited->tiles.size() != edited->num_tiles ||
					edited->uncompressed_data.size() < required_data_size)
				{
					m_import_status = "The current tileset has an inconsistent tile count and cannot be replaced safely.";
					m_result_asset = std::unique_ptr<rom::TileSet>{};
					return;
				}

				for (std::size_t tile_index = 0; tile_index < edited->num_tiles; ++tile_index)
				{
					const int tile_x = static_cast<int>(tile_index % picker_width);
					const int tile_y = static_cast<int>(tile_index / picker_width);
					if (!ReadIndexedTile(*m_preview_image, tile_x, tile_y, pixels, packed))
					{
						m_import_status = "The full tileset PNG could not be read using the exported tile layout.";
						m_result_asset = std::unique_ptr<rom::TileSet>{};
						return;
					}

					edited->tiles[tile_index].pixel_data = pixels;
					const std::size_t byte_offset =
						tile_index * rom::TileSet::s_tile_total_bytes;
					std::copy(
						packed.begin(),
						packed.end(),
						edited->uncompressed_data.begin() +
							static_cast<std::ptrdiff_t>(byte_offset)
					);
				}
				edited->uncompressed_size = static_cast<Uint32>(edited->uncompressed_data.size());
			}

			m_result_asset = std::move(edited);
			result_tileset = std::get<std::unique_ptr<rom::TileSet>>(m_result_asset).get();
			if (result_tileset)
			{
				RenderTileset(*result_tileset);
			}
		}

		if (result_tileset != nullptr && !m_tileset_validation_ready)
		{
			const TilesetCompressionPlan validation = BuildTilesetCompressionPlan(
				m_owning_ui,
				target_tileset,
				*result_tileset
			);
			m_tileset_validation_ready = true;
			m_tileset_validation_valid = validation.valid;
			m_tileset_validation_identical = validation.identical;
			m_tileset_validation_stream_size = validation.stream.size();
			m_tileset_validation_capacity = validation.capacity;
			m_tileset_validation_baseline_size = validation.baseline_size;
			m_tileset_validation_strategy = validation.strategy;
			m_tileset_validation_message = validation.message;
		}

		if (result_tileset != nullptr && m_export_preview_texture != nullptr)
		{
			ImGui::Image(
				(ImTextureID)m_export_preview_texture.get(),
				ImVec2{
					static_cast<float>(m_export_preview_texture->w) * img_scale,
					static_cast<float>(m_export_preview_texture->h) * img_scale
				},
				ImVec2{ 0,0 },
				ImVec2{
					static_cast<float>(m_export_preview_image->w) / m_export_preview_texture->w,
					static_cast<float>(m_export_preview_image->h) / m_export_preview_texture->h
				}
			);

			ImGui::SeparatorText("Import validation");
			ImGui::Text("PNG dimensions: %d x %d pixels - OK", m_preview_image->w, m_preview_image->h);
			ImGui::Text("Compression: %s", CompressionDisplayName(target_tileset.compression_algorithm));
			if (m_tileset_validation_ready && !m_tileset_validation_identical &&
				m_tileset_validation_capacity > 0U)
			{
				ImGui::Text(
					"Compressed size: %zu / %zu bytes",
					m_tileset_validation_stream_size,
					m_tileset_validation_capacity
				);
				if (m_tileset_validation_stream_size <= m_tileset_validation_capacity)
				{
					ImGui::Text(
						"Remaining space: %zu bytes",
						m_tileset_validation_capacity - m_tileset_validation_stream_size
					);
				}
				if (!m_tileset_validation_strategy.empty())
				{
					ImGui::Text("Compression strategy: %s", m_tileset_validation_strategy.c_str());
				}
				if (m_tileset_validation_baseline_size > m_tileset_validation_stream_size)
				{
					ImGui::Text(
						"Compression gain: %zu bytes",
						m_tileset_validation_baseline_size - m_tileset_validation_stream_size
					);
				}
			}

			if (m_tileset_validation_valid && !m_tileset_validation_identical)
			{
				ImGui::TextWrapped("Validation successful: %s", m_tileset_validation_message.c_str());
			}
			else if (m_tileset_validation_identical)
			{
				ImGui::TextWrapped("No import required: %s", m_tileset_validation_message.c_str());
			}
			else
			{
				ImGui::TextWrapped("Validation failed: %s", m_tileset_validation_message.c_str());
			}

			const char* button_label = single_tile_mode
				? "VALIDATE AND IMPORT TILE INTO ROM"
				: "VALIDATE AND IMPORT TILESET INTO ROM";
			ImGui::BeginDisabled(
				!m_tileset_validation_ready ||
				!m_tileset_validation_valid ||
				m_tileset_validation_identical
			);
			if (ImGui::Button(button_label))
			{
				const TilesetWriteResult write_result = WriteTilesetToROM(
					m_owning_ui,
					target_tileset,
					*result_tileset,
					!single_tile_mode
				);
				m_import_status = write_result.message;
				if (write_result.success && write_result.changed)
				{
					TilesetEntry refreshed_entry = rom::TileSet::LoadFromROM(
						m_owning_ui.GetROM(),
						target_tileset.rom_data.rom_offset,
						target_tileset.compression_algorithm
					);
					m_result_asset = std::move(refreshed_entry.tileset);
					RenderTileset(target_tileset);
					m_tileset_validation_ready = false;
					m_force_update_write_location = true;
				}
			}
			ImGui::EndDisabled();
		}
	}

	void EditorImageImporter::DrawSpriteImport()
	{
		rom::Sprite& target_sprite = *std::get<rom::Sprite*>(m_target_asset);

		std::shared_ptr<const rom::Sprite> result_sprite;
		if (auto* result =
			std::get_if<std::shared_ptr<const rom::Sprite>>(&m_result_asset))
		{
			result_sprite = *result;
		}
		ImGui::SetNextItemWidth(256);
		int target_write_location = static_cast<int>(target_sprite.rom_data.rom_offset);
		if (ImGui::InputInt("Target Write Offset", &target_write_location, 1, 100, ImGuiInputTextFlags_CharsHexadecimal | ImGuiInputTextFlags_ReadOnly) || m_force_update_write_location)
		{
			m_force_update_write_location = false;
			m_result_asset =
				rom::Sprite::LoadFromROM(
					m_owning_ui.GetROM(),
					target_write_location
				);

			if (auto* result =
				std::get_if<std::shared_ptr<const rom::Sprite>>(&m_result_asset))
			{
				result_sprite = *result;
			}
			else
			{
				result_sprite.reset();
			}
			if(result_sprite != nullptr)
			{
				m_export_preview_image = SDLSurfaceHandle{ SDL_CreateSurface(result_sprite->GetBoundingBox().Width(), result_sprite->GetBoundingBox().Height(), SDL_PIXELFORMAT_INDEX8) };
				SDL_SetSurfacePalette(m_export_preview_image.get(), m_preview_palette.get());
				Renderer::SetPalette(m_preview_palette);
				result_sprite->RenderToSurface(m_export_preview_image.get());
				m_export_preview_texture = Renderer::RenderToTexture(m_export_preview_image.get());
			}
		}

		if (result_sprite != nullptr)
		{
			ImGui::Image((ImTextureID)m_export_preview_texture.get()
				, ImVec2(static_cast<float>(result_sprite->GetBoundingBox().Width()) * img_scale, static_cast<float>(result_sprite->GetBoundingBox().Height()) * img_scale));


			if (ImGui::Button("/!\\ OVERWRITE SPRITE IN ROM DATA /!\\"))
			{
				rom::Sprite& target_sprite = *std::get<rom::Sprite*>(m_target_asset);

				const BoundingBox bounds = result_sprite->GetBoundingBox();
				rom::SpinballROM& rom = m_owning_ui.GetROM();
				Uint8* current_byte = &rom.m_buffer[target_write_location];
				current_byte += 2; // tiles
				current_byte += 2; // vdp tiles

				for (const std::shared_ptr<rom::SpriteTile>& sprite_tile : result_sprite->sprite_tiles)
				{
					current_byte += 2; // xoffset
					current_byte += 2; // yoffset

					current_byte += 2; // ysize, xsize
				}

				for (const std::shared_ptr<rom::SpriteTile>& sprite_tile : result_sprite->sprite_tiles)
				{
					const SDL_PixelFormatDetails* preview_pixel_format_details = SDL_GetPixelFormatDetails(m_preview_image->format);
					const size_t preview_pitch_offset_per_line = m_preview_image->pitch - (m_preview_image->w * preview_pixel_format_details->bytes_per_pixel);
					size_t preview_pitch_offset = 0;


					const size_t total_pixels = sprite_tile->x_size * sprite_tile->y_size;
					if (total_pixels != 0)
					{
						int x_off = (sprite_tile->x_offset - bounds.min.x);
						int y_off = (sprite_tile->y_offset - bounds.min.y);
						int x_max = x_off + sprite_tile->x_size;
						int y_max = y_off + sprite_tile->y_size;

						size_t pixels_written = 0;
						size_t pixel_source_idx = (y_off * m_preview_image->pitch) + x_off;
						while (pixels_written < total_pixels && pixel_source_idx < m_preview_image->pitch * m_preview_image->h)
						{
							if (pixels_written != 0 && (pixels_written % sprite_tile->x_size) == 0)
							{
								pixel_source_idx = (y_off * m_preview_image->pitch) + (m_preview_image->pitch * (pixels_written / sprite_tile->x_size)) + x_off;
							}
							*current_byte = ((static_cast<Uint8*>(m_preview_image->pixels)[pixel_source_idx] & 0x0F) << 4);
							++pixel_source_idx;
							++pixels_written;

							if (pixels_written != 0 && (pixels_written % sprite_tile->x_size) == 0)
							{
								pixel_source_idx = (y_off * m_preview_image->pitch) + (m_preview_image->pitch * (pixels_written / sprite_tile->x_size)) + x_off;
							}
							*current_byte = *current_byte | static_cast<Uint8*>(m_preview_image->pixels)[pixel_source_idx] & 0x0F;
							++pixel_source_idx;
							++pixels_written;

							++current_byte;
						}
					}
				}
			}
		}
	}

	void EditorImageImporter::Update()
	{
		if (m_visible)
		{
			ImGui::SetNextWindowPos(ImVec2{ 0,16 });
			ImGui::SetNextWindowSize(ImVec2{ Renderer::s_window_width, Renderer::s_window_height - 16 });
			if (ImGui::Begin("Image Importer", &m_visible, ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings))
			{
				InnerUpdate();
			}
			ImGui::End();
		}

		if (!m_tile_import_result.empty())
		{
			constexpr const char* popup_title = "Tile Import Result";
			if (!ImGui::IsPopupOpen(popup_title))
			{
				ImGui::OpenPopup(popup_title);
			}

			bool popup_open = true;
			if (ImGui::BeginPopupModal(
				popup_title,
				&popup_open,
				ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings
			))
			{
				ImGui::PushTextWrapPos(ImGui::GetFontSize() * 42.0f);
				ImGui::TextWrapped("%s", m_tile_import_result.c_str());
				ImGui::PopTextWrapPos();
				ImGui::Separator();
				if (ImGui::Button("OK", ImVec2{ 120.0f, 0.0f }))
				{
					m_tile_import_result.clear();
					ImGui::CloseCurrentPopup();
				}
				ImGui::EndPopup();
			}

			if (!popup_open)
			{
				m_tile_import_result.clear();
			}
		}
	}

	void EditorImageImporter::SetTarget(rom::Sprite& target_sprite)
	{
		m_target_asset = &target_sprite;
		m_result_asset = std::shared_ptr<const rom::Sprite>{};
		m_target_tile_index.reset();
		m_import_status.clear();
		m_tile_import_result.clear();
		m_loaded_path.clear();
		m_imported_image.reset();
		m_preview_image.reset();
		m_export_preview_image.reset();
		m_rendered_imported_image.reset();
		m_rendered_preview_image.reset();
		m_export_preview_texture.reset();
		m_detected_colours.clear();
		m_open_image_selector = false;
		m_close_image_selector = false;
		m_tileset_validation_ready = false;
		m_force_update_write_location = true;
	}

	void EditorImageImporter::SetTarget(
		rom::TileSet& target_tileset,
		std::optional<std::size_t> target_tile_index
	)
	{
		m_target_asset = &target_tileset;
		m_result_asset = std::unique_ptr<rom::TileSet>{};
		m_target_tile_index = target_tile_index;
		m_import_status.clear();
		m_tile_import_result.clear();
		m_loaded_path.clear();
		m_imported_image.reset();
		m_preview_image.reset();
		m_export_preview_image.reset();
		m_rendered_imported_image.reset();
		m_rendered_preview_image.reset();
		m_export_preview_texture.reset();
		m_detected_colours.clear();
		m_open_image_selector = true;
		m_close_image_selector = false;
		m_tileset_validation_ready = false;
		m_tileset_validation_valid = false;
		m_tileset_validation_identical = false;
		m_tileset_validation_stream_size = 0U;
		m_tileset_validation_capacity = 0U;
		m_tileset_validation_baseline_size = 0U;
		m_tileset_validation_strategy.clear();
		m_tileset_validation_message.clear();
		m_force_update_write_location = true;
	}

	void EditorImageImporter::SetAvailablePalettes(const std::vector<std::shared_ptr<rom::Palette>>& palette_lines)
	{
		m_available_palettes.clear();
		std::copy(std::begin(palette_lines), std::end(palette_lines), std::back_inserter(m_available_palettes));
	}

	void EditorImageImporter::SetAvailablePalettes(const rom::PaletteSetArray& palette_set_lines)
	{
		m_available_palettes.clear();
		std::copy(std::begin(palette_set_lines), std::end(palette_set_lines), std::back_inserter(m_available_palettes));
	}

}
