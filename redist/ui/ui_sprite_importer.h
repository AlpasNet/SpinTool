#pragma once

#include "ui/ui_editor_window.h"
#include "ui/ui_palette.h"
#include "rom/palette.h"

#include "rom/spinball_rom.h"
#include "types/sdl_handle_defs.h"
#include "types/rom_ptr.h"

#include "imgui.h"

#include <string>
#include <vector>
#include <memory>
#include <variant>
#include <optional>

namespace spintool
{
	class EditorUI;

	namespace rom
	{
		struct Sprite;
		struct TileSet;
	}
}

namespace spintool
{
	struct ColourPaletteMapping
	{
		ImColor colour;
		Uint8 palette_index;
	};

	class EditorImageImporter : public EditorWindowBase
	{
	public:
		using EditorWindowBase::EditorWindowBase;

		void Update() override;
		void SetTarget(rom::Sprite& target_sprite);
		void SetTarget(
			rom::TileSet& target_tileset,
			std::optional<std::size_t> target_tile_index = std::nullopt
		);
		void SetAvailablePalettes(const std::vector<std::shared_ptr<rom::Palette>>& palette_lines);
		void SetAvailablePalettes(const rom::PaletteSetArray& palette_set_lines);

	private:
		void InnerUpdate();
		void RenderTileset(rom::TileSet& tileset);
		void DrawTileSetImport();
		void DrawSpriteImport();
		SDLSurfaceHandle m_imported_image;
		SDLSurfaceHandle m_preview_image;
		SDLSurfaceHandle m_export_preview_image;
		SDLTextureHandle m_rendered_imported_image;
		SDLTextureHandle m_rendered_preview_image;
		SDLTextureHandle m_export_preview_texture;

		std::vector<ColourPaletteMapping> m_detected_colours;
		std::variant<std::unique_ptr<rom::TileSet>, std::shared_ptr<const rom::Sprite>> m_result_asset;
		std::variant<rom::TileSet*, rom::Sprite*> m_target_asset;
		std::vector<std::shared_ptr<rom::Palette>> m_available_palettes;
		rom::Palette m_selected_palette;
		SDLPaletteHandle m_preview_palette;
		std::string m_loaded_path;

		int m_selected_palette_index = 0;
		bool m_force_update_write_location = false;
		std::optional<std::size_t> m_target_tile_index;
		std::string m_import_status;
		std::string m_tile_import_result;
		bool m_open_image_selector = false;
		bool m_close_image_selector = false;

		bool m_tileset_validation_ready = false;
		bool m_tileset_validation_valid = false;
		bool m_tileset_validation_identical = false;
		std::size_t m_tileset_validation_stream_size = 0U;
		std::size_t m_tileset_validation_capacity = 0U;
		std::size_t m_tileset_validation_baseline_size = 0U;
		std::string m_tileset_validation_strategy;
		std::string m_tileset_validation_message;
	};
}