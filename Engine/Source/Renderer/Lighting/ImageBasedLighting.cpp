#include "Engine/Renderer/Lighting/ImageBasedLighting.h"
#include "Engine/Renderer/Lighting/EnvironmentBaker.h"
#include "Engine/Renderer/Vulkan/upload_context.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <stb_image.h>
#include <tinyexr.h>

namespace Engine {
namespace {
constexpr std::uint32_t EnvironmentSize = 512;
constexpr std::uint32_t IrradianceSize = 32;
constexpr std::uint32_t PrefilterSize = 256;
constexpr std::uint32_t BrdfLutSize = 256;
// Bump this whenever any bake parameter or integration algorithm changes.
constexpr std::uint64_t IblCacheVersion = 4;

enum class IblCacheKind : std::uint32_t { Cubemap = 1 };

struct IblCacheHeader {
    char magic[4];
    std::uint32_t version;
    std::uint32_t kind;
    std::uint32_t size;
    std::uint32_t mipLevels;
    std::uint64_t sourceHash;
    std::uint64_t payloadBytes;
};

static_assert(sizeof(IblCacheHeader) == 40);

std::uint64_t hashBytes(std::uint64_t hash, const std::byte* bytes, const std::size_t size) noexcept {
    constexpr std::uint64_t Prime = 1099511628211ULL;
    for (std::size_t index = 0; index < size; ++index) {
        hash ^= static_cast<std::uint8_t>(bytes[index]);
        hash *= Prime;
    }
    return hash;
}

std::uint64_t hashEnvironment(const std::filesystem::path& path) {
    constexpr std::uint64_t OffsetBasis = 14695981039346656037ULL;
    std::uint64_t hash = hashBytes(OffsetBasis, reinterpret_cast<const std::byte*>(&IblCacheVersion),
                                   sizeof(IblCacheVersion));
    const auto proceduralHash = [&]() {
        return hashBytes(hash, reinterpret_cast<const std::byte*>("procedural"), 10);
    };
    if (path.empty()) return proceduralHash();
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        // Keep the renderer's historical behaviour: a missing/corrupt source
        // is represented by the procedural environment, never a fatal init error.
        std::cerr << "[IBL] Could not open environment for hashing '" << path.string()
                  << "'; using procedural fallback.\n";
        return proceduralHash();
    }
    std::array<std::byte, 64 * 1024> buffer{};
    while (input) {
        input.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
        const auto count = static_cast<std::size_t>(input.gcount());
        hash = hashBytes(hash, buffer.data(), count);
    }
    if (!input.eof()) {
        std::cerr << "[IBL] Could not read environment for hashing '" << path.string()
                  << "'; using procedural fallback.\n";
        return proceduralHash();
    }
    return hash;
}

std::size_t cubemapFloatCount(const std::uint32_t size, const std::uint32_t mipLevels) noexcept {
    std::size_t count = 0;
    for (std::uint32_t mip = 0; mip < mipLevels; ++mip) {
        const auto side = std::max(1U, size >> mip);
        count += static_cast<std::size_t>(6) * side * side * 4;
    }
    return count;
}

bool loadCache(const std::filesystem::path& path, const IblCacheKind kind, const std::uint32_t size,
               const std::uint32_t mipLevels, const std::uint64_t sourceHash, std::vector<std::byte>& payload) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) return false;
    const auto length = input.tellg();
    if (length < static_cast<std::streamoff>(sizeof(IblCacheHeader))) return false;
    input.seekg(0);
    IblCacheHeader header{};
    input.read(reinterpret_cast<char*>(&header), sizeof(header));
    const auto expectedBytes = cubemapFloatCount(size, mipLevels) * sizeof(float);
    if (!input || std::string_view(header.magic, 4) != "GTEX" || header.version != IblCacheVersion ||
        header.kind != static_cast<std::uint32_t>(kind) || header.size != size ||
        header.mipLevels != mipLevels || header.sourceHash != sourceHash || header.payloadBytes != expectedBytes ||
        length != static_cast<std::streamoff>(sizeof(header) + expectedBytes)) return false;
    payload.resize(expectedBytes);
    input.read(reinterpret_cast<char*>(payload.data()), static_cast<std::streamsize>(payload.size()));
    return static_cast<bool>(input);
}

void saveCache(const std::filesystem::path& path, const IblCacheKind kind, const std::uint32_t size,
               const std::uint32_t mipLevels, const std::uint64_t sourceHash, std::span<const std::byte> payload) {
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (error) throw std::runtime_error("Could not create IBL cache directory: " + error.message());
    const auto temporary = path.string() + ".tmp";
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output) throw std::runtime_error("Could not write IBL cache: " + path.string());
    const IblCacheHeader header{{'G', 'T', 'E', 'X'}, static_cast<std::uint32_t>(IblCacheVersion),
        static_cast<std::uint32_t>(kind), size, mipLevels, sourceHash, payload.size()};
    output.write(reinterpret_cast<const char*>(&header), sizeof(header));
    output.write(reinterpret_cast<const char*>(payload.data()), static_cast<std::streamsize>(payload.size()));
    output.close();
    if (!output) throw std::runtime_error("Could not finish IBL cache: " + path.string());
    std::filesystem::rename(temporary, path, error);
    if (error) {
        std::filesystem::remove(path, error);
        error.clear();
        std::filesystem::rename(temporary, path, error);
        if (error) throw std::runtime_error("Could not publish IBL cache: " + error.message());
    }
}

std::filesystem::path cacheDirectory(const std::filesystem::path& libraryDirectory, const std::uint64_t hash) {
    char name[17]{};
    std::snprintf(name, sizeof(name), "%016llx", static_cast<unsigned long long>(hash));
    return libraryDirectory / "IBL" / name;
}

struct BrdfLutHeader {
    char magic[4];
    std::uint32_t version;
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t channels;
    std::uint32_t samples;
};
static_assert(sizeof(BrdfLutHeader) == 24);

std::vector<std::byte> loadBrdfLutAsset(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    const std::size_t payloadBytes = static_cast<std::size_t>(BrdfLutSize) * BrdfLutSize * 2 * sizeof(std::uint16_t);
    if (!input || input.tellg() != static_cast<std::streamoff>(sizeof(BrdfLutHeader) + payloadBytes)) {
        throw std::runtime_error("Missing or invalid cooked BRDF LUT asset: " + path.string());
    }
    input.seekg(0);
    BrdfLutHeader header{};
    input.read(reinterpret_cast<char*>(&header), sizeof(header));
    if (!input || std::string_view(header.magic, 4) != "BRDF" || header.version != 3 ||
        header.width != BrdfLutSize || header.height != BrdfLutSize || header.channels != 2 || header.samples != 1024) {
        throw std::runtime_error("BRDF LUT asset has an incompatible BRDF version: " + path.string());
    }
    std::vector<std::byte> payload(payloadBytes);
    input.read(reinterpret_cast<char*>(payload.data()), static_cast<std::streamsize>(payload.size()));
    if (!input) throw std::runtime_error("Could not read cooked BRDF LUT asset: " + path.string());
    return payload;
}

// Procedural fallback is an HDR *directional* source, not six flat colours.
// Replacing radiance() with HDR/EXR cubemap sampling keeps the baker unchanged.
Vec3 proceduralRadiance(Vec3 direction) {
    const float horizon = std::clamp(direction.y() * 0.5F + 0.5F, 0.0F, 1.0F);
    Vec3 sky = Vec3{0.035F, 0.055F, 0.12F} * (1.0F - horizon) + Vec3{0.22F, 0.48F, 0.95F} * horizon;
    const Vec3 sun = Vec3{0.35F, 0.82F, -0.45F}.normalized();
    const float sunDisk = std::pow(std::max(dot(direction, sun), 0.0F), 2048.0F);
    return sky + Vec3{18.0F, 14.0F, 8.0F} * sunDisk;
}

struct EnvironmentSource {
    struct Mip {
        int width = 0;
        int height = 0;
        std::vector<float> pixels;
    };
    std::vector<Mip> mips;

    [[nodiscard]] bool loaded() const noexcept { return !mips.empty(); }

    [[nodiscard]] Vec3 sample(Vec3 direction, const float mip = 0.0F) const {
        if (!loaded()) return proceduralRadiance(direction);
        const float clampedMip = std::clamp(mip, 0.0F, static_cast<float>(mips.size() - 1));
        const auto sampleMip = [direction](const Mip& level) {
            const int width = level.width;
            const int height = level.height;
            const auto& pixels = level.pixels;
            const float longitude = std::atan2(direction.z(), direction.x());
            const float u = longitude * (0.5F / Pi) + 0.5F;
            const float v = std::acos(std::clamp(direction.y(), -1.0F, 1.0F)) / Pi;
            const float x = u * static_cast<float>(width) - 0.5F;
            const float y = v * static_cast<float>(height) - 0.5F;
            const int x0 = (static_cast<int>(std::floor(x)) % width + width) % width;
            const int x1 = (x0 + 1) % width;
            const int y0 = std::clamp(static_cast<int>(std::floor(y)), 0, height - 1);
            const int y1 = std::min(y0 + 1, height - 1);
            const float tx = x - std::floor(x); const float ty = y - std::floor(y);
            const auto pixel = [&pixels, width](int px, int py) {
                const auto index = (static_cast<std::size_t>(py) * width + px) * 4;
                return Vec3{pixels[index], pixels[index + 1], pixels[index + 2]};
            };
            const Vec3 a = pixel(x0, y0) * (1.0F - tx) + pixel(x1, y0) * tx;
            const Vec3 b = pixel(x0, y1) * (1.0F - tx) + pixel(x1, y1) * tx;
            return a * (1.0F - ty) + b * ty;
        };
        const auto lower = static_cast<std::size_t>(std::floor(clampedMip));
        const auto upper = std::min(lower + 1, mips.size() - 1);
        return sampleMip(mips[lower]) * (1.0F - (clampedMip - static_cast<float>(lower))) +
               sampleMip(mips[upper]) * (clampedMip - static_cast<float>(lower));
    }

    void buildMipChain() {
        while (mips.back().width > 1 || mips.back().height > 1) {
            const Mip& previous = mips.back();
            Mip next{std::max(1, previous.width / 2), std::max(1, previous.height / 2)};
            next.pixels.resize(static_cast<std::size_t>(next.width) * next.height * 4);
            for (int y = 0; y < next.height; ++y) {
                for (int x = 0; x < next.width; ++x) {
                    const int x0 = x * 2, y0 = y * 2;
                    for (int channel = 0; channel < 4; ++channel) {
                        float sum = 0.0F;
                        for (int dy = 0; dy < 2; ++dy) {
                            for (int dx = 0; dx < 2; ++dx) {
                                const int sourceX = (x0 + dx) % previous.width;
                                const int sourceY = std::min(y0 + dy, previous.height - 1);
                                sum += previous.pixels[
                                    (static_cast<std::size_t>(sourceY) * previous.width + sourceX) * 4 + channel];
                            }
                        }
                        next.pixels[(static_cast<std::size_t>(y) * next.width + x) * 4 + channel] = sum * 0.25F;
                    }
                }
            }
            mips.push_back(std::move(next));
        }
    }
};

EnvironmentSource loadEquirectangular(const std::filesystem::path& path) {
    if (path.empty()) return {};
    const auto extension = path.extension().string();
    if (extension == ".exr" || extension == ".EXR") {
        float* decoded = nullptr;
        int width = 0; int height = 0;
        const char* error = nullptr;
        const std::string nativePath = path.string();
        if (LoadEXR(&decoded, &width, &height, nativePath.c_str(), &error) != TINYEXR_SUCCESS ||
            decoded == nullptr || width <= 0 || height <= 0) {
            std::cerr << "[IBL] Could not load EXR panorama '" << path.string() << "': "
                      << (error != nullptr ? error : "invalid EXR") << "; using procedural fallback.\n";
            if (error != nullptr) FreeEXRErrorMessage(error);
            if (decoded != nullptr) free(decoded);
            return {};
        }
        EnvironmentSource source; source.mips.push_back({width, height, {}});
        source.mips.front().pixels.assign(decoded, decoded + static_cast<std::size_t>(width) * height * 4);
        source.buildMipChain();
        free(decoded);
        return source;
    }
    int width = 0; int height = 0; int channels = 0;
    const std::string nativePath = path.string();
    float* const decoded = stbi_loadf(nativePath.c_str(), &width, &height, &channels, 4);
    if (decoded == nullptr || width <= 0 || height <= 0) {
        std::cerr << "[IBL] Could not load HDR panorama '" << path.string() << "': "
                  << stbi_failure_reason() << "; using procedural fallback.\n";
        if (decoded != nullptr) stbi_image_free(decoded);
        return {};
    }
    EnvironmentSource source; source.mips.push_back({width, height, {}});
    source.mips.front().pixels.assign(decoded, decoded + static_cast<std::size_t>(width) * height * 4);
    source.buildMipChain();
    stbi_image_free(decoded);
    return source;
}

}

void ImageBasedLighting::create(VkPhysicalDevice physicalDevice, VkDevice device, VkCommandPool commandPool,
                                VkQueue queue, VmaAllocator allocator,
                                const std::filesystem::path& equirectangularPath,
                                const std::filesystem::path& libraryDirectory,
                                const IblQualitySettings quality) {
    const auto environmentHash = hashEnvironment(equirectangularPath);
    const auto iblDirectory = cacheDirectory(libraryDirectory, environmentHash);
    const auto environmentCache = iblDirectory / "environment.gtex";
    const auto irradianceCache = iblDirectory / "irradiance.gtex";
    const auto prefilteredCache = iblDirectory / "prefiltered.gtex";
    const auto brdfAsset = libraryDirectory.parent_path() / "Assets" / "BRDF" / "brdf_lut.bin";
    // Build every resource away from the live set. A decode, allocation or
    // upload failure must leave the active descriptor targets intact.
    ImageBasedLighting replacement;
    const auto environmentMips = std::bit_width(quality.environmentResolution);
    const auto prefilterMips = std::bit_width(quality.prefilterResolution);
    std::vector<std::byte> payload;
    std::optional<UploadContext::Batch> uploadBatch;
    if (auto* upload = UploadContext::current(); upload != nullptr && !upload->recording())
        uploadBatch.emplace(upload->beginBatch());
    const auto uploadCubemap = [&](const std::filesystem::path& path, const std::uint32_t size,
                                   const std::uint32_t mipLevels, const auto& bake, Cubemap& target) {
        if (loadCache(path, IblCacheKind::Cubemap, size, mipLevels, environmentHash, payload)) {
            std::cerr << "[IBL] Cache hit: " << path.string() << '\n';
        } else {
            std::cerr << "[IBL] Cache miss; baking " << path.string() << '\n';
            const auto pixels = bake();
            payload.resize(pixels.size() * sizeof(float));
            std::memcpy(payload.data(), pixels.data(), payload.size());
            saveCache(path, IblCacheKind::Cubemap, size, mipLevels, environmentHash, payload);
        }
        // std::vector<std::byte> is not required to provide float alignment;
        // copy into typed storage before handing it to the cubemap uploader.
        std::vector<float> pixels(payload.size() / sizeof(float));
        std::memcpy(pixels.data(), payload.data(), payload.size());
        target.createHdr(physicalDevice, device, commandPool, queue, size, mipLevels, pixels);
    };

    std::optional<EnvironmentSource> source;
    const auto getSource = [&]() -> const EnvironmentSource& {
        if (!source) source.emplace(loadEquirectangular(equirectangularPath));
        return *source;
    };
    uploadCubemap(environmentCache, quality.environmentResolution, environmentMips, [&] {
        const auto& decoded = getSource();
        return EnvironmentBaker::bakeCubemap(quality.environmentResolution, environmentMips,
            [&decoded](const Vec3& direction, const std::uint32_t mip, std::uint32_t) {
                return decoded.sample(direction, static_cast<float>(mip));
            });
    }, replacement.environment_);
    uploadCubemap(irradianceCache, IrradianceSize, 1, [&] {
        const auto& decoded = getSource();
        return EnvironmentBaker::bakeCubemap(IrradianceSize, 1, [&decoded](const Vec3& direction, std::uint32_t, std::uint32_t) {
            return EnvironmentBaker::diffuseIrradiance(direction,
                [&decoded](const Vec3& sampleDirection) { return decoded.sample(sampleDirection); });
        });
    }, replacement.irradiance_);
    uploadCubemap(prefilteredCache, quality.prefilterResolution, prefilterMips, [&] {
        const auto& decoded = getSource();
        return EnvironmentBaker::bakePrefilteredCubemap(quality.prefilterResolution, prefilterMips, quality.environmentResolution,
            environmentMips, [&decoded](const Vec3& direction, const float mip) {
                return decoded.sample(direction, mip);
            });
    }, replacement.prefiltered_);

    payload = loadBrdfLutAsset(brdfAsset);
    const auto bytes = std::span<const std::uint8_t>{reinterpret_cast<const std::uint8_t*>(payload.data()), payload.size()};
    replacement.brdfLut_.create(physicalDevice, device, commandPool, queue, BrdfLutSize, BrdfLutSize, bytes,
                                TextureColorSpace::Linear, false, allocator, TexturePixelFormat::RG16F);
    if (uploadBatch) static_cast<void>(uploadBatch->submit());
    swap(replacement);
}

void ImageBasedLighting::destroy() noexcept { brdfLut_.destroy(); prefiltered_.destroy(); irradiance_.destroy(); environment_.destroy(); }

void ImageBasedLighting::swap(ImageBasedLighting& other) noexcept {
    using std::swap;
    swap(environment_, other.environment_);
    swap(irradiance_, other.irradiance_);
    swap(prefiltered_, other.prefiltered_);
    swap(brdfLut_, other.brdfLut_);
}

std::array<VkDescriptorImageInfo, 3> ImageBasedLighting::descriptors() const noexcept {
    return {{{irradiance_.sampler(), irradiance_.imageView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
             {prefiltered_.sampler(), prefiltered_.imageView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
             {brdfLut_.sampler(), brdfLut_.imageView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}}};
}

VkDescriptorImageInfo ImageBasedLighting::environmentDescriptor() const noexcept {
    return {environment_.sampler(), environment_.imageView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
}
} // namespace Engine
