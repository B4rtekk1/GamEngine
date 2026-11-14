#include "DdsImage.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <limits>

namespace Engine::Assets {
namespace {
std::uint32_t word(const std::uint8_t* data) {
    return std::uint32_t{data[0]} | (std::uint32_t{data[1]} << 8) | (std::uint32_t{data[2]} << 16) |
           (std::uint32_t{data[3]} << 24);
}

std::array<std::uint8_t, 4> color565(const std::uint16_t value) {
    const auto r = static_cast<std::uint8_t>((value >> 11) & 31);
    const auto g = static_cast<std::uint8_t>((value >> 5) & 63);
    const auto b = static_cast<std::uint8_t>(value & 31);
    return {static_cast<std::uint8_t>((r << 3) | (r >> 2)), static_cast<std::uint8_t>((g << 2) | (g >> 4)),
            static_cast<std::uint8_t>((b << 3) | (b >> 2)), 255};
}

std::array<std::uint8_t, 16> alpha_block(const std::uint8_t* block) {
    std::array<std::uint8_t, 8> palette{};
    palette[0] = block[0];
    palette[1] = block[1];
    if (block[0] > block[1]) {
        for (unsigned i = 2; i < 8; ++i)
            palette[i] = static_cast<std::uint8_t>(((8 - i) * block[0] + (i - 1) * block[1]) / 7);
    } else {
        for (unsigned i = 2; i < 6; ++i)
            palette[i] = static_cast<std::uint8_t>(((6 - i) * block[0] + (i - 1) * block[1]) / 5);
        palette[6] = 0;
        palette[7] = 255;
    }
    std::uint64_t bits{};
    for (unsigned i = 0; i < 6; ++i)
        bits |= std::uint64_t{block[2 + i]} << (8 * i);
    std::array<std::uint8_t, 16> result{};
    for (unsigned i = 0; i < 16; ++i)
        result[i] = palette[(bits >> (3 * i)) & 7];
    return result;
}

void color_block(const std::uint8_t* block, const bool opaque, std::array<std::array<std::uint8_t, 4>, 16>& pixels) {
    const auto a = static_cast<std::uint16_t>(block[0] | (block[1] << 8));
    const auto b = static_cast<std::uint16_t>(block[2] | (block[3] << 8));
    std::array<std::array<std::uint8_t, 4>, 4> palette{color565(a), color565(b), {}, {}};
    if (a > b || opaque) {
        for (unsigned channel = 0; channel < 3; ++channel) {
            palette[2][channel] = static_cast<std::uint8_t>((2 * palette[0][channel] + palette[1][channel]) / 3);
            palette[3][channel] = static_cast<std::uint8_t>((palette[0][channel] + 2 * palette[1][channel]) / 3);
        }
        palette[2][3] = palette[3][3] = 255;
    } else {
        for (unsigned channel = 0; channel < 3; ++channel)
            palette[2][channel] = static_cast<std::uint8_t>((palette[0][channel] + palette[1][channel]) / 2);
        palette[2][3] = 255;
    }
    const auto indices = word(block + 4);
    for (unsigned i = 0; i < 16; ++i)
        pixels[i] = palette[(indices >> (2 * i)) & 3];
}
} // namespace

namespace {
struct DdsLayout {
    DdsBlockInfo info;
    std::size_t payloadOffset{};
};

[[nodiscard]] std::optional<DdsLayout> parse_dds_layout(const std::uint8_t* bytes, const std::size_t size) {
    if (size < 128 || word(bytes) != 0x20534444 || word(bytes + 4) != 124 || word(bytes + 76) != 32)
        return std::nullopt;
    DdsLayout layout;
    layout.info.height = word(bytes + 12);
    layout.info.width = word(bytes + 16);
    // DDSD_MIPMAPCOUNT; without it the file holds the top level only.
    layout.info.mipCount = (word(bytes + 8) & 0x20000U) != 0 ? std::max(word(bytes + 28), 1U) : 1U;
    if (layout.info.width == 0 || layout.info.height == 0 || layout.info.width > 16384 ||
        layout.info.height > 16384 || layout.info.mipCount > 15)
        return std::nullopt;
    const auto fourcc = word(bytes + 84);
    layout.payloadOffset = 128;
    if (fourcc == 0x31545844)
        layout.info.format = DdsBlockFormat::BC1; // DXT1
    else if (fourcc == 0x35545844)
        layout.info.format = DdsBlockFormat::BC3; // DXT5
    else if (fourcc == 0x32495441 || fourcc == 0x55354342)
        layout.info.format = DdsBlockFormat::BC5; // ATI2 / BC5U
    else if (fourcc == 0x30315844 && size >= 148) { // DX10
        layout.payloadOffset = 148;
        const auto dxgi = word(bytes + 128);
        if (dxgi == 71 || dxgi == 72)
            layout.info.format = DdsBlockFormat::BC1;
        else if (dxgi == 77 || dxgi == 78)
            layout.info.format = DdsBlockFormat::BC3;
        else if (dxgi == 83 || dxgi == 84)
            layout.info.format = DdsBlockFormat::BC5;
        else
            return std::nullopt;
    } else
        return std::nullopt;
    std::uint32_t fullChain = 1;
    for (std::uint32_t extent = std::max(layout.info.width, layout.info.height); extent > 1; extent /= 2)
        ++fullChain;
    layout.info.completeMipChain = layout.info.mipCount == fullChain;
    return layout;
}
} // namespace

std::optional<DdsBlockInfo> read_dds_block_info(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    std::array<std::uint8_t, 148> header{};
    input.read(reinterpret_cast<char*>(header.data()), static_cast<std::streamsize>(header.size()));
    const auto layout = parse_dds_layout(header.data(), static_cast<std::size_t>(input.gcount()));
    if (!layout)
        return std::nullopt;
    return layout->info;
}

std::optional<CookedTexture> read_dds_blocks(const std::filesystem::path& path, const TextureFormat format) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input)
        return std::nullopt;
    const auto length = input.tellg();
    if (length < 128)
        return std::nullopt;
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(length));
    input.seekg(0);
    input.read(reinterpret_cast<char*>(bytes.data()), length);
    if (!input)
        return std::nullopt;
    const auto layout = parse_dds_layout(bytes.data(), bytes.size());
    if (!layout)
        return std::nullopt;
    const DdsBlockFormat expected = format == TextureFormat::BC1_UNORM || format == TextureFormat::BC1_SRGB
                                        ? DdsBlockFormat::BC1
                                    : format == TextureFormat::BC3_UNORM || format == TextureFormat::BC3_SRGB
                                        ? DdsBlockFormat::BC3
                                    : format == TextureFormat::BC5_UNORM ? DdsBlockFormat::BC5
                                                                         : DdsBlockFormat{0xff};
    if (layout->info.format != expected)
        return std::nullopt;
    CookedTexture texture;
    texture.width = layout->info.width;
    texture.height = layout->info.height;
    texture.format = format;
    const std::uint32_t blockBytes = textureFormatBlockBytes(format);
    std::uint64_t offset = layout->payloadOffset;
    std::uint32_t width = texture.width;
    std::uint32_t height = texture.height;
    for (std::uint32_t mip = 0; mip < layout->info.mipCount; ++mip) {
        const std::uint64_t mipBytes = std::uint64_t{(width + 3) / 4} * ((height + 3) / 4) * blockBytes;
        if (offset + mipBytes > bytes.size())
            return std::nullopt;
        texture.mips.push_back({.offset = texture.data.size(), .size = mipBytes, .width = width, .height = height});
        texture.data.insert(texture.data.end(), bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                            bytes.begin() + static_cast<std::ptrdiff_t>(offset + mipBytes));
        offset += mipBytes;
        width = std::max(1U, width / 2);
        height = std::max(1U, height / 2);
    }
    return texture;
}

bool decode_dds_image(const std::filesystem::path& path, std::vector<std::uint8_t>& rgba, std::uint32_t& width,
                      std::uint32_t& height) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input)
        return false;
    const auto length = input.tellg();
    if (length < 128)
        return false;
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(length));
    input.seekg(0);
    input.read(reinterpret_cast<char*>(bytes.data()), length);
    if (!input || word(bytes.data()) != 0x20534444 || word(bytes.data() + 4) != 124 || word(bytes.data() + 76) != 32)
        return false;
    height = word(bytes.data() + 12);
    width = word(bytes.data() + 16);
    if (!width || !height || width > 16384 || height > 16384 ||
        std::uint64_t{width} * height * 4 > 512ULL * 1024 * 1024)
        return false;
    const auto fourcc = word(bytes.data() + 84);
    enum class Format { BC1, BC3, BC5 } format;
    std::size_t offset = 128;
    if (fourcc == 0x31545844)
        format = Format::BC1; // DXT1
    else if (fourcc == 0x35545844)
        format = Format::BC3; // DXT5
    else if (fourcc == 0x32495441 || fourcc == 0x55354342)
        format = Format::BC5;                               // ATI2 / BC5U
    else if (fourcc == 0x30315844 && bytes.size() >= 148) { // DX10
        offset = 148;
        const auto dxgi = word(bytes.data() + 128);
        if (dxgi == 71 || dxgi == 72)
            format = Format::BC1;
        else if (dxgi == 77 || dxgi == 78)
            format = Format::BC3;
        else if (dxgi == 83 || dxgi == 84)
            format = Format::BC5;
        else
            return false;
    } else
        return false;
    const auto blocksX = (std::size_t{width} + 3) / 4;
    const auto blocksY = (std::size_t{height} + 3) / 4;
    const auto stride = format == Format::BC1 ? 8u : 16u;
    if (blocksX * blocksY > (bytes.size() - offset) / stride)
        return false;
    rgba.resize(static_cast<std::size_t>(width) * height * 4);
    const auto* block = bytes.data() + offset;
    for (std::size_t by = 0; by < blocksY; ++by) {
        for (std::size_t bx = 0; bx < blocksX; ++bx, block += stride) {
            std::array<std::array<std::uint8_t, 4>, 16> pixels{};
            if (format == Format::BC1)
                color_block(block, false, pixels);
            else if (format == Format::BC3) {
                color_block(block + 8, true, pixels);
                const auto alpha = alpha_block(block);
                for (unsigned i = 0; i < 16; ++i)
                    pixels[i][3] = alpha[i];
            } else {
                const auto red = alpha_block(block);
                const auto green = alpha_block(block + 8);
                for (unsigned i = 0; i < 16; ++i) {
                    const float x = red[i] / 127.5F - 1.0F;
                    const float y = green[i] / 127.5F - 1.0F;
                    const float z = std::sqrt(std::max(0.0F, 1.0F - x * x - y * y));
                    pixels[i] = {red[i], green[i], static_cast<std::uint8_t>((z + 1.0F) * 127.5F), 255};
                }
            }
            for (unsigned y = 0; y < 4; ++y)
                for (unsigned x = 0; x < 4; ++x) {
                    const auto px = bx * 4 + x, py = by * 4 + y;
                    if (px >= width || py >= height)
                        continue;
                    const auto dst = (py * width + px) * 4;
                    for (unsigned channel = 0; channel < 4; ++channel)
                        rgba[dst + channel] = pixels[y * 4 + x][channel];
                }
        }
    }
    return true;
}
} // namespace Engine::Assets
