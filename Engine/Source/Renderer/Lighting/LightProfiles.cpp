#include "Engine/Renderer/Lighting/LightProfiles.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace Engine {
namespace {
constexpr float Pi = 3.14159265358979323846F;

[[nodiscard]] float smooth(const float edge0, const float edge1, const float x) noexcept {
    const float t = std::clamp((x - edge0) / (edge1 - edge0), 0.0F, 1.0F);
    return t * t * (3.0F - 2.0F * t);
}

// Relative intensity of a built-in fixture at `degrees` from its forward
// (downward) axis.
[[nodiscard]] float builtInIntensity(const LightProfile profile, const float degrees) noexcept {
    const float cosine = std::cos(degrees * Pi / 180.0F);
    switch (profile) {
    case LightProfile::Downlight:
        // Recessed reflector: a ~35 degree beam with a soft shoulder and a
        // trace of spill so the ceiling is not pitch black.
        return std::max(std::pow(std::max(cosine, 0.0F), 6.0F), 0.02F);
    case LightProfile::Lampshade: {
        // Fabric shade open at both ends: a bright pool below, the lit shade
        // glowing sideways and a softer opening upwards.
        const float down = 1.0F - smooth(40.0F, 70.0F, degrees);
        const float up = smooth(120.0F, 150.0F, degrees) * 0.6F;
        return std::max({down, up, 0.22F});
    }
    case LightProfile::Sconce: {
        // Wall sconce with an open bowl: up- and downlight scallops on the
        // wall, dimmer straight out.
        const float lobe = std::pow(std::abs(cosine), 1.5F);
        return std::max(cosine >= 0.0F ? lobe : 0.8F * lobe, 0.12F);
    }
    case LightProfile::Lantern: {
        // Street lantern: glass sides light the street widely (a batwing
        // peaking near 60 degrees) under an opaque roof.
        const float batwing = 0.75F + 0.25F * std::sin(std::clamp(degrees, 0.0F, 90.0F) * Pi / 90.0F);
        return std::max(batwing * (1.0F - smooth(95.0F, 125.0F, degrees)), 0.04F);
    }
    case LightProfile::Globe:
        // Bare bulb in a frosted globe: even all round except the socket.
        return 1.0F - 0.7F * smooth(140.0F, 180.0F, degrees);
    case LightProfile::None:
    case LightProfile::Ies:
        break;
    }
    return 1.0F;
}

[[nodiscard]] LightProfileTable quantize(const std::array<float, LightProfileSamples>& values) {
    const float peak = *std::ranges::max_element(values);
    LightProfileTable table{};
    for (std::size_t index = 0; index < LightProfileSamples; ++index)
        table[index] = static_cast<std::uint8_t>(
            std::lround(std::clamp(peak > 0.0F ? values[index] / peak : 0.0F, 0.0F, 1.0F) * 255.0F));
    return table;
}

[[nodiscard]] float sampleDegrees(const std::size_t index) noexcept {
    return 180.0F * static_cast<float>(index) / static_cast<float>(LightProfileSamples - 1);
}
} // namespace

const char* lightProfileName(const LightProfile profile) noexcept {
    switch (profile) {
    case LightProfile::None:
        return "None";
    case LightProfile::Downlight:
        return "Downlight";
    case LightProfile::Lampshade:
        return "Lampshade";
    case LightProfile::Sconce:
        return "Wall sconce";
    case LightProfile::Lantern:
        return "Street lantern";
    case LightProfile::Globe:
        return "Globe bulb";
    case LightProfile::Ies:
        return "IES file";
    }
    return "None";
}

LightProfileTable builtInLightProfile(const LightProfile profile) noexcept {
    std::array<float, LightProfileSamples> values{};
    for (std::size_t index = 0; index < LightProfileSamples; ++index)
        values[index] = builtInIntensity(profile, sampleDegrees(index));
    return quantize(values);
}

std::optional<LightProfileTable> parseIesProfile(const std::string_view text) {
    // Skip the header and keyword lines up to TILT=.
    std::size_t cursor = 0;
    std::string tilt;
    while (cursor < text.size()) {
        const std::size_t end = std::min(text.find('\n', cursor), text.size());
        std::string_view line = text.substr(cursor, end - cursor);
        cursor = end + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
            line.remove_suffix(1);
        if (line.starts_with("TILT=")) {
            tilt = std::string{line.substr(5)};
            break;
        }
    }
    if (tilt.empty() || cursor >= text.size())
        return std::nullopt;

    // The rest is whitespace/comma separated numbers.
    std::vector<double> numbers;
    {
        std::string body{text.substr(cursor)};
        std::ranges::replace(body, ',', ' ');
        std::istringstream stream{body};
        double value = 0.0;
        while (stream >> value)
            numbers.push_back(value);
    }
    std::size_t at = 0;
    const auto take = [&]() -> std::optional<double> {
        if (at >= numbers.size())
            return std::nullopt;
        return numbers[at++];
    };
    if (tilt == "INCLUDE") {
        // Lamp-to-luminaire geometry, pair count, angles and multipliers.
        if (!take())
            return std::nullopt;
        const auto pairs = take();
        if (!pairs || *pairs < 0.0 || *pairs > 10000.0)
            return std::nullopt;
        at += static_cast<std::size_t>(*pairs) * 2U;
    } else if (tilt != "NONE") {
        // TILT=<file>: the tilt data only adjusts output for lamp tilt; the
        // distribution below is still complete.
    }
    std::array<double, 13> header{};
    for (double& value : header) {
        const auto read = take();
        if (!read)
            return std::nullopt;
        value = *read;
    }
    const double multiplier = header[2];
    const auto verticalCount = static_cast<std::size_t>(header[3]);
    const auto horizontalCount = static_cast<std::size_t>(header[4]);
    if (verticalCount < 2 || horizontalCount < 1 || verticalCount > 4096 || horizontalCount > 4096)
        return std::nullopt;
    std::vector<double> vertical(verticalCount);
    for (double& angle : vertical) {
        const auto read = take();
        if (!read)
            return std::nullopt;
        angle = *read;
    }
    if (!std::ranges::is_sorted(vertical))
        return std::nullopt;
    at += horizontalCount; // horizontal angles: averaged over below
    std::vector<double> candela(verticalCount, 0.0);
    for (std::size_t horizontal = 0; horizontal < horizontalCount; ++horizontal)
        for (std::size_t index = 0; index < verticalCount; ++index) {
            const auto read = take();
            if (!read)
                return std::nullopt;
            candela[index] += std::max(*read, 0.0) * std::abs(multiplier) / static_cast<double>(horizontalCount);
        }

    // Resample: angles outside the measured range emit nothing (a downlight
    // measured 0-90 is dark above the horizon).
    std::array<float, LightProfileSamples> values{};
    for (std::size_t index = 0; index < LightProfileSamples; ++index) {
        const double degrees = sampleDegrees(index);
        if (degrees < vertical.front() - 1.0e-6 || degrees > vertical.back() + 1.0e-6)
            continue;
        const auto upper = std::ranges::upper_bound(vertical, degrees);
        const std::size_t high = std::min<std::size_t>(static_cast<std::size_t>(upper - vertical.begin()),
                                                       verticalCount - 1);
        const std::size_t low = high == 0 ? 0 : high - 1;
        const double span = vertical[high] - vertical[low];
        const double t = span > 1.0e-9 ? std::clamp((degrees - vertical[low]) / span, 0.0, 1.0) : 0.0;
        values[index] = static_cast<float>(candela[low] + (candela[high] - candela[low]) * t);
    }
    if (*std::ranges::max_element(values) <= 0.0F)
        return std::nullopt;
    return quantize(values);
}

std::optional<LightProfileTable> loadIesProfile(const std::filesystem::path& path) {
    std::ifstream file{path, std::ios::binary};
    if (!file)
        return std::nullopt;
    std::ostringstream contents;
    contents << file.rdbuf();
    return parseIesProfile(contents.str());
}

std::array<glm::uvec4, 2> packLightProfile(const LightProfileTable& table) noexcept {
    std::array<glm::uvec4, 2> packed{};
    for (std::size_t index = 0; index < LightProfileSamples; ++index) {
        auto& word = packed[index / 16][static_cast<glm::length_t>((index % 16) / 4)];
        word |= static_cast<std::uint32_t>(table[index]) << ((index % 4) * 8U);
    }
    return packed;
}
} // namespace Engine
