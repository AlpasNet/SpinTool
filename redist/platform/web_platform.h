#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace spintool::web
{
    [[nodiscard]] bool IsWebBuild();

    // Opens the browser's native file picker. The selected file is copied into
    // Emscripten's virtual filesystem under target_directory.
    [[nodiscard]] int RequestFile(
        const std::vector<std::string>& extensions,
        const std::filesystem::path& target_directory
    );

    // Returns the selected virtual path once the asynchronous browser picker
    // has completed. A null optional means that it is still pending or that it
    // was cancelled. Use FileRequestFinished() to distinguish both cases.
    [[nodiscard]] std::optional<std::filesystem::path> PollSelectedFile(int request_id);
    [[nodiscard]] bool FileRequestFinished(int request_id);
    void ForgetFileRequest(int request_id);

    // Flushes the /spintool IDBFS mount to IndexedDB in web builds.
    void SyncPersistentStorage();

    // Downloads a file from Emscripten's virtual filesystem.
    [[nodiscard]] bool DownloadFile(
        const std::filesystem::path& path,
        const std::string& mime_type,
        const std::string& download_name = {}
    );
}
