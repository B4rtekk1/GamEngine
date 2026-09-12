#include "Engine/Assets/Gtex.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <limits>

namespace Engine::Assets {
    namespace {
        constexpr std::uint32_t kVersion = 1;
        constexpr std::array<char, 4> kMagic = {'G', 'T', 'E', 'X'};

        struct FileHeader {
            char magic[4];
            std::uint32_t version;
            std::uint32_t width;
            std::uint32_t height;
            std::uint32_t mipCount;
            std::uint32_t format;
        };

        struct FileMip {
            std::uint32_t width;
            std::uint32_t height;
            std::uint64_t offset;
            std::uint64_t size;
        };

        static_assert(sizeof(FileHeader) == 24);
        static_assert(sizeof(FileMip) == 24);

        [[nodiscard]] bool supported_format(const TextureFormat format) noexcept {
            return format >= TextureFormat::RGBA8_SRGB && format <= TextureFormat::BC7_SRGB;
        }

        [[nodiscard]] std::uint64_t mip_size(const TextureFormat format, const std::uint32_t width,
                                             const std::uint32_t height) noexcept {
            if (width == 0 || height == 0) return 0;
            switch (format) {
                case TextureFormat::RGBA8_SRGB:
                case TextureFormat::RGBA8_UNORM:
                    return static_cast<std::uint64_t>(width) * height * 4;
                case TextureFormat::BC4_UNORM:
                    return static_cast<std::uint64_t>((width + 3) / 4) * ((height + 3) / 4) * 8;
                case TextureFormat::BC5_UNORM:
                case TextureFormat::BC7_UNORM:
                case TextureFormat::BC7_SRGB:
                    return static_cast<std::uint64_t>((width + 3) / 4) * ((height + 3) / 4) * 16;
            }
            return 0;
        }
    }

    bool valid_cooked_texture(const CookedTexture &texture) noexcept {
        if (texture.width == 0 || texture.height == 0 || texture.mips.empty() || !supported_format(texture.format))
            return false;
        std::uint32_t expectedWidth = texture.width;
        std::uint32_t expectedHeight = texture.height;
        for (const TextureMip &mip: texture.mips) {
            if (mip.width != expectedWidth || mip.height != expectedHeight || mip.size != mip_size(
                    texture.format, mip.width, mip.height) ||
                mip.offset > texture.data.size() || mip.size > texture.data.size() - mip.offset)
                return false;
            expectedWidth = std::max(1U, expectedWidth / 2);
            expectedHeight = std::max(1U, expectedHeight / 2);
        }
        return true;
    }

    std::optional<GtexTexture> load_gtex(const std::filesystem::path &path) {
        std::ifstream stream(path, std::ios::binary | std::ios::ate);
        if (!stream) return std::nullopt;
        const auto length = stream.tellg();
        if (length < static_cast<std::streamoff>(sizeof(FileHeader))) return std::nullopt;
        stream.seekg(0);
        FileHeader header{};
        stream.read(reinterpret_cast<char *>(&header), sizeof(header));
        if (!stream || !std::equal(kMagic.begin(), kMagic.end(), header.magic) || header.version != kVersion ||
            header.mipCount == 0 || header.mipCount > 32 || header.format > static_cast<std::uint32_t>(
                TextureFormat::BC7_SRGB))
            return std::nullopt;
        const std::uint64_t tableBytes = static_cast<std::uint64_t>(header.mipCount) * sizeof(FileMip);
        const std::uint64_t headerBytes = sizeof(FileHeader) + tableBytes;
        if (headerBytes > static_cast<std::uint64_t>(length)) return std::nullopt;
        std::vector<FileMip> entries(header.mipCount);
        stream.read(reinterpret_cast<char *>(entries.data()), static_cast<std::streamsize>(tableBytes));
        if (!stream) return std::nullopt;
        GtexTexture texture;
        texture.path = path;
        texture.width = header.width;
        texture.height = header.height;
        texture.format = static_cast<TextureFormat>(header.format);
        texture.payloadOffset = headerBytes;
        texture.mips.reserve(entries.size());
        for (const FileMip &entry: entries) texture.mips.push_back(
            {entry.offset, entry.size, entry.width, entry.height});
        const std::uint64_t payloadSize = static_cast<std::uint64_t>(length) - headerBytes;
        std::uint32_t expectedWidth = texture.width;
        std::uint32_t expectedHeight = texture.height;
        for (const TextureMip &mip: texture.mips) {
            if (mip.width != expectedWidth || mip.height != expectedHeight ||
                mip.size != mip_size(texture.format, mip.width, mip.height) || mip.offset > payloadSize ||
                mip.size > payloadSize - mip.offset)
                return std::nullopt;
            expectedWidth = std::max(1U, expectedWidth / 2);
            expectedHeight = std::max(1U, expectedHeight / 2);
        }
        return texture;
    }

    bool readMipRange(const GtexTexture &texture, const std::uint32_t mip, const std::uint64_t offset,
                      const std::span<std::uint8_t> destination) {
        if (mip >= texture.mips.size()) return false;
        const TextureMip &entry = texture.mips[mip];
        if (offset > entry.size || destination.size() > entry.size - offset) return false;
        std::ifstream stream(texture.path, std::ios::binary);
        if (!stream) return false;
        const auto absolute = texture.payloadOffset + entry.offset + offset;
        if (absolute > static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max())) return false;
        stream.seekg(static_cast<std::streamoff>(absolute));
        stream.read(reinterpret_cast<char *>(destination.data()), static_cast<std::streamsize>(destination.size()));
        return static_cast<bool>(stream);
    }

    bool save_gtex(const std::filesystem::path &path, const CookedTexture &texture) {
        if (!valid_cooked_texture(texture) || texture.mips.size() > std::numeric_limits<std::uint32_t>::max()) return
                false;
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        if (!stream) return false;
        const FileHeader header{
            {'G', 'T', 'E', 'X'}, kVersion, texture.width, texture.height,
            static_cast<std::uint32_t>(texture.mips.size()), static_cast<std::uint32_t>(texture.format)
        };
        stream.write(reinterpret_cast<const char *>(&header), sizeof(header));
        for (const TextureMip &mip: texture.mips) {
            const FileMip entry{mip.width, mip.height, mip.offset, mip.size};
            stream.write(reinterpret_cast<const char *>(&entry), sizeof(entry));
        }
        stream.write(reinterpret_cast<const char *>(texture.data.data()),
                     static_cast<std::streamsize>(texture.data.size()));
        return static_cast<bool>(stream);
    }
}
