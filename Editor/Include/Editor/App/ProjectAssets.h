#pragma once

#include <filesystem>
#include <stdexcept>

namespace Editor {

// Projects need the same cooked lighting asset shipped with the Editor.
inline void ensureProjectBrdfLut(const std::filesystem::path& assetRoot, const std::filesystem::path& editorRoot) {
    const auto destination = assetRoot / "BRDF" / "brdf_lut.bin";
    if (std::filesystem::exists(destination))
        return;

    const auto source = editorRoot / "Assets" / "BRDF" / "brdf_lut.bin";
    if (!std::filesystem::is_regular_file(source)) {
        throw std::runtime_error("Bundled BRDF LUT asset is missing: " + source.string());
    }
    std::filesystem::create_directories(destination.parent_path());
    std::filesystem::copy_file(source, destination, std::filesystem::copy_options::skip_existing);
}

} // namespace Editor
