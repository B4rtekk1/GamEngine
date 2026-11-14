#pragma once

#include "Engine/Scene/Components/LightComponent.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string_view>

#include <glm/glm.hpp>

namespace Engine {
/**
 * A light's photometric profile as the GPU consumes it: relative intensity
 * (peak 255) at LightProfileSamples equal steps of the angle from the light's
 * forward axis, 0 to 180 degrees. Common/SceneData.slang samples it in
 * localLightProfile().
 */
inline constexpr std::size_t LightProfileSamples = 32;
using LightProfileTable = std::array<std::uint8_t, LightProfileSamples>;

/** Display name of a profile for editor UI. */
[[nodiscard]] const char* lightProfileName(LightProfile profile) noexcept;

/** Table of a built-in profile; None and Ies give a uniform (all 255) table. */
[[nodiscard]] LightProfileTable builtInLightProfile(LightProfile profile) noexcept;

/**
 * Parses an IESNA LM-63 (1986/1991/1995/2002) photometric file. Candela
 * values are averaged over the horizontal angles (the forward axis is the
 * fixture's nadir), resampled to the table and normalised to its peak.
 * Returns nothing for malformed files or files without light.
 */
[[nodiscard]] std::optional<LightProfileTable> parseIesProfile(std::string_view text);
[[nodiscard]] std::optional<LightProfileTable> loadIesProfile(const std::filesystem::path& path);

/** Packs a table into the two uint4 words of the GPU light record. */
[[nodiscard]] std::array<glm::uvec4, 2> packLightProfile(const LightProfileTable& table) noexcept;
} // namespace Engine
