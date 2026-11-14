#include "Engine/Core/PngWriter.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <fstream>

namespace Engine {
namespace {
std::uint32_t crc32(const std::uint8_t* data, const std::size_t size) {
    static const auto table = [] {
        std::array<std::uint32_t, 256> result{};
        for (std::uint32_t n = 0; n < 256; ++n) {
            std::uint32_t c = n;
            for (int k = 0; k < 8; ++k)
                c = (c & 1U) != 0 ? 0xEDB88320U ^ (c >> 1U) : c >> 1U;
            result[n] = c;
        }
        return result;
    }();
    std::uint32_t crc = ~0U;
    for (std::size_t i = 0; i < size; ++i)
        crc = table[(crc ^ data[i]) & 0xFFU] ^ (crc >> 8U);
    return ~crc;
}

void appendBigEndian(std::vector<std::uint8_t>& out, const std::uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8)
        out.push_back(static_cast<std::uint8_t>(value >> shift));
}

void appendChunk(std::vector<std::uint8_t>& out, const char (&type)[5], const std::vector<std::uint8_t>& body) {
    appendBigEndian(out, static_cast<std::uint32_t>(body.size()));
    std::vector<std::uint8_t> typed(type, type + 4);
    typed.insert(typed.end(), body.begin(), body.end());
    out.insert(out.end(), typed.begin(), typed.end());
    appendBigEndian(out, crc32(typed.data(), typed.size()));
}
} // namespace

bool writePngRgb8(const std::filesystem::path& path, const std::uint32_t width, const std::uint32_t height,
                  const std::vector<std::uint8_t>& rgb) {
    const std::size_t rowBytes = std::size_t{width} * 3;
    if (width == 0 || height == 0 || rgb.size() < rowBytes * height)
        return false;
    std::vector<std::uint8_t> raw;
    raw.reserve((rowBytes + 1) * height);
    for (std::uint32_t y = 0; y < height; ++y) {
        raw.push_back(0); // filter: none
        const auto row = rgb.begin() + static_cast<std::ptrdiff_t>(rowBytes * y);
        raw.insert(raw.end(), row, row + static_cast<std::ptrdiff_t>(rowBytes));
    }
    // zlib stream made of stored (uncompressed) deflate blocks.
    std::vector<std::uint8_t> zlib{0x78, 0x01};
    std::uint32_t a = 1, b = 0;
    for (const std::uint8_t byte : raw) {
        a = (a + byte) % 65521U;
        b = (b + a) % 65521U;
    }
    for (std::size_t offset = 0; offset < raw.size(); offset += 65535) {
        const auto length = static_cast<std::uint16_t>(std::min<std::size_t>(65535, raw.size() - offset));
        zlib.push_back(offset + length >= raw.size() ? 1 : 0);
        zlib.push_back(static_cast<std::uint8_t>(length & 0xFFU));
        zlib.push_back(static_cast<std::uint8_t>(length >> 8U));
        zlib.push_back(static_cast<std::uint8_t>(~length & 0xFFU));
        zlib.push_back(static_cast<std::uint8_t>((~length >> 8U) & 0xFFU));
        zlib.insert(zlib.end(), raw.begin() + static_cast<std::ptrdiff_t>(offset),
                    raw.begin() + static_cast<std::ptrdiff_t>(offset + length));
    }
    appendBigEndian(zlib, (b << 16U) | a);

    std::vector<std::uint8_t> png{0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    std::vector<std::uint8_t> header;
    appendBigEndian(header, width);
    appendBigEndian(header, height);
    header.insert(header.end(), {8, 2, 0, 0, 0}); // 8-bit RGB
    appendChunk(png, "IHDR", header);
    appendChunk(png, "IDAT", zlib);
    appendChunk(png, "IEND", {});

    std::error_code error;
    if (path.has_parent_path())
        std::filesystem::create_directories(path.parent_path(), error);
    std::ofstream output{path, std::ios::binary | std::ios::trunc};
    if (!output)
        return false;
    output.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
    return static_cast<bool>(output);
}
} // namespace Engine
