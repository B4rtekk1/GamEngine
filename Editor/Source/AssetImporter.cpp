#include "Editor/AssetImporter.h"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <stdexcept>
#include <system_error>

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
    for (const auto& entry : std::filesystem::recursive_directory_iterator(source, error)) {
        if (error) throw std::runtime_error("Could not enumerate imported directory: " + error.message());
        const auto relative = entry.path().lexically_relative(source);
        if (relative.empty() || relative.is_absolute() || relative.string().starts_with("..")) {
            throw std::runtime_error("Archive entry would leave the destination directory");
        }
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

#ifdef _WIN32
std::wstring psQuote(std::wstring value) {
    std::wstring escaped;
    escaped.reserve(value.size() + 2);
    for (const wchar_t character : value) {
        escaped += character;
        if (character == L'\'') escaped += L'\'';
    }
    return L"'" + escaped + L"'";
}

void extractZip(const std::filesystem::path& source, const std::filesystem::path& destination) {
    const auto command = L"powershell.exe -NoProfile -NonInteractive -Command \"Expand-Archive -LiteralPath " +
                         psQuote(source.wstring()) + L" -DestinationPath " + psQuote(destination.wstring()) +
                         L" -Force\"";
    if (_wsystem(command.c_str()) != 0) throw std::runtime_error("Could not extract ZIP archive");
}
#endif

} // namespace

bool import(const std::filesystem::path& source, const std::filesystem::path& destination,
            std::string& error) {
    try {
        if (!std::filesystem::exists(source)) throw std::runtime_error("Selected asset does not exist");
        std::filesystem::create_directories(destination);
        if (std::filesystem::is_regular_file(source) && source.extension() != ".zip") {
            std::filesystem::copy_file(source, uniqueDestination(destination, source.filename()));
        } else if (std::filesystem::is_directory(source)) {
            const auto target = uniqueDestination(destination, source.filename());
            std::filesystem::create_directories(target);
            copyTree(source, target);
        } else if (std::filesystem::is_regular_file(source)) {
#ifdef _WIN32
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
#else
            throw std::runtime_error("ZIP import is currently supported by the Windows editor only");
#endif
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
