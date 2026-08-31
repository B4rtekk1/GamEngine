#pragma once

#include <filesystem>
#include <string>

namespace Editor::AssetImporter {

/** Imports a file, directory tree, or .zip archive into @p destination. */
bool import(const std::filesystem::path& source, const std::filesystem::path& destination,
            std::string& error);

} // namespace Editor::AssetImporter
