#pragma once

#include <filesystem>
#include <string>

enum class EnvironmentTool {
    Ytdlp,
    Ffmpeg,
    Node,
};

struct EnvironmentToolDetection {
    bool available{};
    bool portable{};
    bool portableInstalled{};
    std::filesystem::path path;
};

std::filesystem::path PortableToolsDirectory();
std::filesystem::path PortableYtdlpPath();
std::filesystem::path PortableFfmpegPath();
std::filesystem::path PortableFfprobePath();
std::filesystem::path PortableNodePath();
EnvironmentToolDetection DetectEnvironmentTool(EnvironmentTool tool);

// Returns the PATH value inherited by child processes. Existing entries are
// preserved, while MPVBridge-local tool folders take precedence.
std::wstring PortableChildPathValue();

class ScopedPortableChildPath {
public:
    ScopedPortableChildPath();
    ~ScopedPortableChildPath();
    ScopedPortableChildPath(const ScopedPortableChildPath&) = delete;
    ScopedPortableChildPath& operator=(const ScopedPortableChildPath&) = delete;

private:
    std::wstring previous_;
    bool existed_{};
};
