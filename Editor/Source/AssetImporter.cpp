#include "Editor/AssetImporter.h"

#include <chrono>
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <system_error>
#include <vector>
#include <miniz.h>

namespace Editor::AssetImporter {
namespace {

std::filesystem::path uniqueDestination(const std::filesystem::path& directory,
                                        const std::filesystem::path& filename) {
    auto result = directory / filename;
    for (std::uint32_t number = 1; std::filesystem::exists(result); ++number) {
        result = directory / (filename.stem().string() + " (" + std::to_string(number) + ")" +
                              filename.extension().string());
    }
    return result;
}

void copyTree(const std::filesystem::path& source, const std::filesystem::path& destination) {
    std::error_code error;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(source)) {
        const auto relative = entry.path().lexically_relative(source);
        if (relative.empty() || relative.is_absolute() ||
            std::ranges::find(relative, "..") != relative.end()) {
            throw std::runtime_error("Archive entry would leave the destination directory");
        }
        if (entry.is_symlink()) throw std::runtime_error("Cannot import symbolic links: " + entry.path().string());
        const auto target = destination / relative;
        if (entry.is_directory()) {
            std::filesystem::create_directories(target, error);
        } else if (entry.is_regular_file()) {
            std::filesystem::create_directories(target.parent_path(), error);
            if (!error) std::filesystem::copy_file(entry.path(), uniqueDestination(target.parent_path(), target.filename()),
                                                    std::filesystem::copy_options::none, error);
        }
        if (error) throw std::runtime_error("Could not import asset: " + error.message());
    }
}

void extractZip(const std::filesystem::path& source, const std::filesystem::path& destination) {
    // FILE* preserves Unicode paths on Windows as well as UTF-8 paths on Linux.
#ifdef _WIN32
    FILE* file = _wfopen(source.c_str(), L"rb");
#else
    FILE* file = std::fopen(source.c_str(), "rb");
#endif
    if (!file) throw std::runtime_error("Could not open ZIP archive");
    mz_zip_archive zip{};
    if (!mz_zip_reader_init_cfile(&zip, file, 0, 0)) {
        std::fclose(file);
        throw std::runtime_error("Could not read ZIP archive");
    }
    try {
        std::vector<std::filesystem::path> paths;
        const auto count = mz_zip_reader_get_num_files(&zip);
        for (mz_uint index = 0; index < count; ++index) {
            mz_zip_archive_file_stat stat{};
            if (!mz_zip_reader_file_stat(&zip, index, &stat) || stat.m_is_encrypted || !stat.m_is_supported)
                throw std::runtime_error("Unsupported or encrypted ZIP entry");
            const auto length = mz_zip_reader_get_filename(&zip, index, nullptr, 0);
            std::vector<char> name(length);
            if (length == 0 || mz_zip_reader_get_filename(&zip, index, name.data(), length) != length)
                throw std::runtime_error("Could not read ZIP entry name");
            if (name.back() != '\0' || std::find(name.begin(), name.end() - 1, '\0') != name.end() - 1)
                throw std::runtime_error("Invalid ZIP entry name");
            std::string portableName{name.data()};
            std::ranges::replace(portableName, '\\', '/');
            const auto relative = std::filesystem::path{
                std::u8string{portableName.begin(), portableName.end()}};
            const auto type = (stat.m_external_attr >> 16) & 0170000;
            if (relative.empty() || relative.is_absolute() || relative.has_root_name() ||
                portableName.find(':') != std::string::npos ||
                std::ranges::find(relative, "..") != relative.end() ||
                (type != 0 && type != 0100000 && type != 0040000))
                throw std::runtime_error("Unsafe ZIP entry: " + portableName);
            paths.push_back(relative);
        }
        // Validate the complete archive before extracting any entry.
        for (mz_uint index = 0; index < count; ++index) {
            const auto target = destination / paths[index];
            if (mz_zip_reader_is_file_a_directory(&zip, index)) {
                std::filesystem::create_directories(target);
                continue;
            }
            std::filesystem::create_directories(target.parent_path());
#ifdef _WIN32
            FILE* output = _wfopen(target.c_str(), L"wb");
#else
            FILE* output = std::fopen(target.c_str(), "wb");
#endif
            if (!output) throw std::runtime_error("Could not create extracted file");
            const bool extracted = mz_zip_reader_extract_to_cfile(&zip, index, output, 0) != 0;
            const bool closed = std::fclose(output) == 0;
            if (!extracted || !closed) throw std::runtime_error("Could not extract ZIP entry: " + paths[index].string());
        }
    } catch (...) {
        mz_zip_reader_end(&zip);
        std::fclose(file);
        throw;
    }
    mz_zip_reader_end(&zip);
    std::fclose(file);
}

} // namespace

bool import(const std::filesystem::path& source, const std::filesystem::path& destination,
            std::string& error) {
    try {
        if (!std::filesystem::exists(source)) throw std::runtime_error("Selected asset does not exist");
        if (std::filesystem::is_symlink(source)) throw std::runtime_error("Cannot import symbolic links");
        if (std::filesystem::is_directory(source)) {
            const auto relative = std::filesystem::weakly_canonical(destination).lexically_relative(
                std::filesystem::canonical(source));
            if (!relative.empty() && *relative.begin() != "..")
                throw std::runtime_error("Cannot import a folder into itself or one of its subfolders");
        }
        std::filesystem::create_directories(destination);
        auto extension = source.extension().string();
        std::ranges::transform(extension, extension.begin(), [](const unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
        if (std::filesystem::is_regular_file(source) && extension != ".zip") {
            std::filesystem::copy_file(source, uniqueDestination(destination, source.filename()));
        } else if (std::filesystem::is_directory(source)) {
            const auto target = uniqueDestination(destination, source.filename());
            std::filesystem::create_directories(target);
            copyTree(source, target);
        } else if (std::filesystem::is_regular_file(source)) {
            const auto temporary = destination / (".import-" + std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()));
            std::filesystem::create_directories(temporary);
            try {
                extractZip(source, temporary);
                copyTree(temporary, destination);
                std::filesystem::remove_all(temporary);
            } catch (...) {
                std::error_code ignored;
                std::filesystem::remove_all(temporary, ignored);
                throw;
            }
        } else {
            throw std::runtime_error("Unsupported import source");
        }
        error.clear();
        return true;
    } catch (const std::exception& exception) {
        error = exception.what();
        return false;
    }
}

} // namespace Editor::AssetImporter
