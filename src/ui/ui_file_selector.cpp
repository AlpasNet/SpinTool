#include "ui/ui_file_selector.h"

#include "ui/ui_editor.h"
#include "ui/ui_image_io.h"

#include "imgui.h"

#include "sdl_handle_defs.h"

#include "SDL3/SDL_image.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <system_error>
#include <utility>

#if defined(_WIN32)
#include <windows.h>
#include <commdlg.h>
#endif

namespace spintool
{

namespace
{

bool ExtensionMatches(
    const std::filesystem::path& filepath,
    const std::vector<std::string>& extensions
)
{
    std::string file_extension = PathToUtf8(filepath.extension());
    std::transform(
        file_extension.begin(),
        file_extension.end(),
        file_extension.begin(),
        [](const unsigned char character)
        {
            return static_cast<char>(std::tolower(character));
        }
    );

    return std::any_of(
        extensions.begin(),
        extensions.end(),
        [&file_extension](std::string expected_extension)
        {
            std::transform(
                expected_extension.begin(),
                expected_extension.end(),
                expected_extension.begin(),
                [](const unsigned char character)
                {
                    return static_cast<char>(std::tolower(character));
                }
            );
            return file_extension == expected_extension;
        }
    );
}

#if defined(_WIN32)
std::wstring BuildNativeFilter(const FileSelectorSettings& settings)
{
    std::wstring patterns;
    for (std::size_t index = 0; index < settings.file_extension_filter.size(); ++index)
    {
        if (index != 0)
        {
            patterns.push_back(L';');
        }
        patterns.push_back(L'*');
        for (const char character : settings.file_extension_filter[index])
        {
            patterns.push_back(static_cast<wchar_t>(
                static_cast<unsigned char>(character)
            ));
        }
    }
    if (patterns.empty())
    {
        patterns = L"*.*";
    }

    std::wstring filter = L"Supported files (";
    filter += patterns;
    filter += L")";
    filter.push_back(L'\0');
    filter += patterns;
    filter.push_back(L'\0');
    filter += L"All files (*.*)";
    filter.push_back(L'\0');
    filter += L"*.*";
    filter.push_back(L'\0');
    filter.push_back(L'\0');
    return filter;
}

std::optional<std::filesystem::path> OpenNativeFileDialog(
    const FileSelectorSettings& settings
)
{
    std::vector<wchar_t> selected_path(32768, L'\0');
    const std::wstring filter = BuildNativeFilter(settings);

    std::error_code path_error;
    const std::filesystem::path absolute_directory =
        std::filesystem::absolute(settings.target_directory, path_error);
    const std::wstring initial_directory = path_error
        ? settings.target_directory.wstring()
        : absolute_directory.wstring();

    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.lpstrFile = selected_path.data();
    dialog.nMaxFile = static_cast<DWORD>(selected_path.size());
    dialog.lpstrFilter = filter.c_str();
    dialog.nFilterIndex = 1;
    dialog.lpstrInitialDir = initial_directory.empty()
        ? nullptr
        : initial_directory.c_str();
    dialog.Flags = OFN_EXPLORER |
        OFN_FILEMUSTEXIST |
        OFN_PATHMUSTEXIST |
        OFN_NOCHANGEDIR;

    if (GetOpenFileNameW(&dialog) == FALSE)
    {
        return std::nullopt;
    }

    const std::filesystem::path result{selected_path.data()};
    if (!ExtensionMatches(result, settings.file_extension_filter))
    {
        return std::nullopt;
    }
    return result;
}
#endif

struct FileSelectorEntry
{
    std::filesystem::path filepath{};
    SDLTextureHandle thumbnail{};
};

}

std::optional<std::filesystem::path> DrawFileSelector(
    const FileSelectorSettings& settings,
    EditorUI& owning_ui,
    std::optional<std::filesystem::path> current_selection
)
{
    (void)owning_ui;
#if defined(_WIN32)
    if (settings.use_native_dialog)
    {
        if (settings.open_popup && settings.close_popup == false)
        {
            return OpenNativeFileDialog(settings);
        }
        return std::nullopt;
    }
#endif

    static std::vector<FileSelectorEntry> s_file_entries;
    static std::optional<std::filesystem::path> s_highlighted_path;

    std::optional<std::filesystem::path> return_path;
    const std::string popup_title = "Choose " + settings.object_typename;

    if (settings.open_popup && settings.close_popup == false)
    {
        ImGui::OpenPopup(
            popup_title.c_str(),
            ImGuiPopupFlags_AnyPopupLevel
        );

        s_highlighted_path = current_selection;

        s_file_entries.clear();
        std::error_code directory_error;
        std::filesystem::directory_iterator directory_file_it{
            settings.target_directory,
            directory_error
        };
        if (!directory_error)
        {
            for (const std::filesystem::directory_entry& entry : directory_file_it)
            {
                if (!entry.is_regular_file(directory_error))
                {
                    directory_error.clear();
                    continue;
                }

                const std::filesystem::path filepath = entry.path();
                if (!ExtensionMatches(filepath, settings.file_extension_filter))
                {
                    continue;
                }

                FileSelectorEntry new_entry{filepath};
                if (settings.tiled_previews)
                {
                    SDLSurfaceHandle preview_surface = LoadImageFromPath(filepath);
                    if (preview_surface)
                    {
                        new_entry.thumbnail = SDLTextureHandle{
                            SDL_CreateTextureFromSurface(
                                Renderer::s_renderer,
                                preview_surface.get()
                            )
                        };
                    }
                    if (new_entry.thumbnail != nullptr)
                    {
                        SDL_SetTextureScaleMode(
                            new_entry.thumbnail.get(),
                            SDL_ScaleMode::SDL_SCALEMODE_NEAREST
                        );
                    }
                }
                s_file_entries.emplace_back(std::move(new_entry));
            }
        }
    }

    if (ImGui::IsPopupOpen(popup_title.c_str()))
    {
        ImGui::SetNextWindowPos(
            ImGui::GetMainViewport()->GetCenter(),
            ImGuiCond_Appearing,
            ImVec2(0.5f, 0.5f)
        );
    }

    if (ImGui::BeginPopup(
        popup_title.c_str(),
        ImGuiWindowFlags_Modal
    ))
    {
        if (settings.close_popup)
        {
            s_highlighted_path.reset();
            ImGui::CloseCurrentPopup();
        }

        const std::string target_directory_utf8 =
            PathToUtf8(settings.target_directory);

        ImGui::Text(
            "%s",
            target_directory_utf8.c_str()
        );

        ImGui::Separator();

        if (ImGui::BeginChild(
            "file_list",
            ImVec2{-1, 800}
        ))
        {
            bool has_files = false;
            unsigned int file_index = 0;
            bool group_open = false;

            for (const FileSelectorEntry& file_entry : s_file_entries)
            {
                if (!ExtensionMatches(
                    file_entry.filepath,
                    settings.file_extension_filter
                ))
                {
                    continue;
                }

                const bool is_selected =
                    s_highlighted_path == file_entry.filepath;

                has_files = true;

                if (file_entry.thumbnail != nullptr)
                {
                    if ((file_index % settings.num_columns) == 0)
                    {
                        ImGui::BeginGroup();
                        group_open = true;
                    }
                    else
                    {
                        ImGui::SameLine();
                    }

                    ImGui::BeginGroup();

                    {
                        constexpr float ideal_scale = 2.0f;
                        constexpr ImVec2 ideal_dimensions{128, 128};

                        ImVec2 dimensions{
                            static_cast<float>(
                                file_entry.thumbnail->w
                            ),
                            static_cast<float>(
                                file_entry.thumbnail->h
                            )
                        };

                        if (dimensions.x >= dimensions.y)
                        {
                            if (dimensions.x < ideal_dimensions.x)
                            {
                                dimensions *= ideal_scale;
                            }

                            if (dimensions.x > ideal_dimensions.x)
                            {
                                dimensions =
                                    dimensions *
                                    (
                                        ideal_dimensions.x /
                                        dimensions.x
                                    );
                            }
                        }
                        else
                        {
                            if (dimensions.x < ideal_dimensions.y)
                            {
                                dimensions *= ideal_scale;
                            }

                            if (dimensions.y > ideal_dimensions.y)
                            {
                                dimensions =
                                    dimensions *
                                    (
                                        ideal_dimensions.y /
                                        dimensions.y
                                    );
                            }
                        }

                        const float cursor_pos_x =
                            ImGui::GetCursorPosX();

                        ImGui::SetCursorPosX(
                            cursor_pos_x +
                            (
                                ideal_dimensions.x -
                                dimensions.x
                            ) * 0.5f
                        );

                        ImGui::Image(
                            reinterpret_cast<ImTextureID>(
                                file_entry.thumbnail.get()
                            ),
                            dimensions
                        );

                        const std::string filename_utf8 =
                            PathToUtf8(
                                file_entry.filepath.filename()
                            );

                        ImGui::PushTextWrapPos(
                            cursor_pos_x +
                            ideal_dimensions.x
                        );

                        ImGui::TextWrapped(
                            "%s",
                            filename_utf8.c_str()
                        );

                        ImGui::PopTextWrapPos();
                    }

                    ImGui::EndGroup();

                    if (is_selected)
                    {
                        ImGui::GetCurrentWindow()
                            ->DrawList
                            ->AddRect(
                                ImGui::GetItemRectMin() -
                                    ImVec2{1, 1},
                                ImGui::GetItemRectMax() +
                                    ImVec2{1, 1},
                                ImGui::GetColorU32(
                                    ImVec4{
                                        1.0f,
                                        0.0f,
                                        1.0f,
                                        1.0f
                                    }
                                ),
                                0.0f,
                                ImDrawFlags_None,
                                2.0f
                            );
                    }

                    ++file_index;
                }
                else
                {
                    bool selected = is_selected;

                    const std::string filename_utf8 =
                        PathToUtf8(
                            file_entry.filepath.filename()
                        );

                    ImGui::Selectable(
                        filename_utf8.c_str(),
                        &selected,
                        ImGuiSelectableFlags_DontClosePopups
                    );
                }

                if (ImGui::IsItemClicked(
                    ImGuiMouseButton_Left
                ))
                {
                    s_highlighted_path =
                        file_entry.filepath;
                }

                if (
                    ImGui::IsItemClicked(
                        ImGuiMouseButton_Left
                    ) &&
                    ImGui::IsMouseDoubleClicked(
                        ImGuiMouseButton_Left
                    )
                )
                {
                    return_path = s_highlighted_path;
                }

                if (
                    group_open &&
                    (file_index % settings.num_columns) == 0
                )
                {
                    ImGui::EndGroup();
                    group_open = false;
                }
            }

            if (group_open)
            {
                ImGui::EndGroup();
                group_open = false;
            }

            if (has_files == false)
            {
                ImGui::Text(
                    "<No valid %s detected>",
                    settings.object_typename.c_str()
                );

                const std::filesystem::path path_diff =
                    std::filesystem::relative(
                        settings.target_directory,
                        std::filesystem::current_path()
                    );

                const std::string path_diff_utf8 =
                    PathToUtf8(path_diff);

                ImGui::Text(
                    "<Add to the %s folder in the executable directory>",
                    path_diff_utf8.c_str()
                );
            }
        }

        ImGui::EndChild();
        ImGui::Separator();

        ImGui::BeginDisabled(
            s_highlighted_path.has_value() == false
        );

        const std::string selection_str =
            "Choose " + settings.object_typename;

        if (
            ImGui::Button(selection_str.c_str()) &&
            s_highlighted_path.has_value()
        )
        {
            return_path = s_highlighted_path;
        }

        ImGui::EndDisabled();
        ImGui::SameLine();

        if (ImGui::Button("Cancel"))
        {
            s_highlighted_path.reset();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    return return_path;
}

}
