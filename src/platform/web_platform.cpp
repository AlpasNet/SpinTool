#include "platform/web_platform.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>
#include <algorithm>
#include <cctype>
#include <map>
#include <sstream>

#if defined(__EMSCRIPTEN__)
#include <emscripten/emscripten.h>
#include <emscripten/html5.h>
#endif

namespace spintool::web
{
#if defined(__EMSCRIPTEN__)
    namespace
    {
        struct FileRequestState
        {
            bool finished = false;
            std::optional<std::filesystem::path> selected_path;
        };

        std::map<int, FileRequestState> g_file_requests;
        int g_next_request_id = 1;

        std::string BuildAcceptFilter(const std::vector<std::string>& extensions)
        {
            std::ostringstream accept;
            bool first = true;
            for (std::string extension : extensions)
            {
                if (extension.empty())
                {
                    continue;
                }
                if (extension.front() != '.')
                {
                    extension.insert(extension.begin(), '.');
                }
                std::transform(
                    extension.begin(),
                    extension.end(),
                    extension.begin(),
                    [](unsigned char value)
                    {
                        return static_cast<char>(std::tolower(value));
                    }
                );
                if (!first)
                {
                    accept << ',';
                }
                accept << extension;
                first = false;
            }
            return accept.str();
        }
    }

    extern "C" EMSCRIPTEN_KEEPALIVE void spintool_web_file_selected(
        int request_id,
        const char* virtual_path
    )
    {
        auto request = g_file_requests.find(request_id);
        if (request == g_file_requests.end())
        {
            return;
        }

        request->second.finished = true;
        if (virtual_path != nullptr && virtual_path[0] != '\0')
        {
            request->second.selected_path = std::filesystem::path{virtual_path};
        }
    }

    extern "C" EMSCRIPTEN_KEEPALIVE void spintool_web_file_cancelled(int request_id)
    {
        auto request = g_file_requests.find(request_id);
        if (request != g_file_requests.end())
        {
            request->second.finished = true;
            request->second.selected_path.reset();
        }
    }

    EM_JS(void, OpenBrowserFilePicker, (
        int request_id,
        const char* accept_utf8,
        const char* target_directory_utf8
    ), {
        window.SpinToolWeb.openFilePicker(
            request_id,
            UTF8ToString(accept_utf8),
            UTF8ToString(target_directory_utf8)
        );
    });

    EM_JS(void, SyncBrowserFilesystem, (), {
        window.SpinToolWeb.syncFilesystem();
    });

    EM_JS(int, DownloadBrowserFile, (
        const char* path_utf8,
        const char* mime_type_utf8,
        const char* download_name_utf8
    ), {
        return window.SpinToolWeb.downloadFile(
            UTF8ToString(path_utf8),
            UTF8ToString(mime_type_utf8),
            UTF8ToString(download_name_utf8)
        ) ? 1 : 0;
    });
#endif

    bool IsWebBuild()
    {
#if defined(__EMSCRIPTEN__)
        return true;
#else
        return false;
#endif
    }

    int RequestFile(
        const std::vector<std::string>& extensions,
        const std::filesystem::path& target_directory
    )
    {
#if defined(__EMSCRIPTEN__)
        const int request_id = g_next_request_id++;
        g_file_requests.emplace(request_id, FileRequestState{});
        const std::string accept = BuildAcceptFilter(extensions);
        const std::string target = target_directory.generic_string();
        OpenBrowserFilePicker(request_id, accept.c_str(), target.c_str());
        return request_id;
#else
        (void)extensions;
        (void)target_directory;
        return 0;
#endif
    }

    std::optional<std::filesystem::path> PollSelectedFile(int request_id)
    {
#if defined(__EMSCRIPTEN__)
        const auto request = g_file_requests.find(request_id);
        if (request == g_file_requests.end() || !request->second.finished)
        {
            return std::nullopt;
        }
        return request->second.selected_path;
#else
        (void)request_id;
        return std::nullopt;
#endif
    }

    bool FileRequestFinished(int request_id)
    {
#if defined(__EMSCRIPTEN__)
        const auto request = g_file_requests.find(request_id);
        return request != g_file_requests.end() && request->second.finished;
#else
        (void)request_id;
        return true;
#endif
    }

    void ForgetFileRequest(int request_id)
    {
#if defined(__EMSCRIPTEN__)
        g_file_requests.erase(request_id);
#else
        (void)request_id;
#endif
    }

    void SyncPersistentStorage()
    {
#if defined(__EMSCRIPTEN__)
        SyncBrowserFilesystem();
#endif
    }

    bool DownloadFile(
        const std::filesystem::path& path,
        const std::string& mime_type,
        const std::string& download_name
    )
    {
#if defined(__EMSCRIPTEN__)
        const std::string virtual_path = path.generic_string();
        const std::string filename = download_name.empty()
            ? path.filename().string()
            : download_name;
        return DownloadBrowserFile(
            virtual_path.c_str(),
            mime_type.c_str(),
            filename.c_str()
        ) != 0;
#else
        (void)path;
        (void)mime_type;
        (void)download_name;
        return false;
#endif
    }
}
