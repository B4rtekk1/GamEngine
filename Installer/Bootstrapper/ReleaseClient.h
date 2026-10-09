#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

struct ReleaseAsset {
    std::string version;
    std::wstring url;
    std::string sha256;
    std::uint64_t size = 0;
};

ReleaseAsset latestEditorRelease();
void downloadEditor(const ReleaseAsset& asset, const std::filesystem::path& destination,
                    const std::function<void(int)>& progress);
void validateArchive(const std::filesystem::path& archive);
void extractArchive(const std::filesystem::path& archive, const std::filesystem::path& destination);
