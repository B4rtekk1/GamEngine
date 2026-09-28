#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

namespace Engine::Assets {
    /** Decode the top mip of a BC1, BC3, or BC5 DDS into RGBA8. */
    [[nodiscard]] bool decode_dds_image(const std::filesystem::path& path,
                                        std::vector<std::uint8_t>& rgba,
                                        std::uint32_t& width, std::uint32_t& height);
}
