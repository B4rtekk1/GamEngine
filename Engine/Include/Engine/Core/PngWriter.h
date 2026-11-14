#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

namespace Engine {
/**
 * Writes 8-bit RGB pixels (row-major, three bytes each) as an uncompressed
 * PNG. Creates the parent directory; returns false when it cannot write.
 */
[[nodiscard]] bool writePngRgb8(const std::filesystem::path& path, std::uint32_t width, std::uint32_t height,
                                const std::vector<std::uint8_t>& rgb);
} // namespace Engine
