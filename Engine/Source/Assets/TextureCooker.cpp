#include "Engine/Assets/TextureCooker.h"
#include "Engine/Assets/Gtex.h"

#include <cmp_core.h>
#include <stb_image.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cctype>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

namespace Engine::Assets {
    namespace {
        [[nodiscard]] float srgb_to_linear(const std::uint8_t value) noexcept {
            const float encoded = static_cast<float>(value) / 255.0F;
            return encoded <= 0.04045F ? encoded / 12.92F : std::pow((encoded + 0.055F) / 1.055F, 2.4F);
        }

        [[nodiscard]] std::uint8_t linear_to_srgb(const float value) noexcept {
            const float linear = std::clamp(value, 0.0F, 1.0F);
            const float encoded = linear <= 0.0031308F ? linear * 12.92F : 1.055F * std::pow(linear, 1.0F / 2.4F) - 0.055F;
            return static_cast<std::uint8_t>(std::round(encoded * 255.0F));
        }

        [[nodiscard]] std::vector<std::uint8_t> downsample_rgba(const std::span<const std::uint8_t> source,
                                                                 const std::uint32_t sourceWidth,
                                                                 const std::uint32_t sourceHeight,
                                                                 const bool srgb) {
            const std::uint32_t width = std::max(1U, sourceWidth / 2);
            const std::uint32_t height = std::max(1U, sourceHeight / 2);
            std::vector<std::uint8_t> result(static_cast<std::size_t>(width) * height * 4);
            for (std::uint32_t y = 0; y < height; ++y) for (std::uint32_t x = 0; x < width; ++x) {
                std::array<float, 4> sum{};
                for (std::uint32_t oy = 0; oy < 2; ++oy) for (std::uint32_t ox = 0; ox < 2; ++ox) {
                    const auto sx = std::min(sourceWidth - 1, x * 2 + ox);
                    const auto sy = std::min(sourceHeight - 1, y * 2 + oy);
                    const auto offset = (static_cast<std::size_t>(sy) * sourceWidth + sx) * 4;
                    for (std::uint32_t channel = 0; channel < 3; ++channel)
                        sum[channel] += srgb ? srgb_to_linear(source[offset + channel]) : static_cast<float>(source[offset + channel]);
                    sum[3] += static_cast<float>(source[offset + 3]);
                }
                const auto output = (static_cast<std::size_t>(y) * width + x) * 4;
                for (std::uint32_t channel = 0; channel < 3; ++channel)
                    result[output + channel] = srgb ? linear_to_srgb(sum[channel] / 4.0F) : static_cast<std::uint8_t>(std::round(sum[channel] / 4.0F));
                result[output + 3] = static_cast<std::uint8_t>(std::round(sum[3] / 4.0F));
            }
            return result;
        }

        void encode_mip_bc7(const std::span<const std::uint8_t> rgba, const std::uint32_t width,
                            const std::uint32_t height, std::vector<std::uint8_t>& destination) {
            const std::uint32_t blocksWide = (width + 3) / 4;
            const std::uint32_t blocksHigh = (height + 3) / 4;
            const std::uint32_t blockCount = blocksWide * blocksHigh;
            destination.resize(destination.size() + static_cast<std::size_t>(blockCount) * 16);
            const auto output = destination.data() + destination.size() - static_cast<std::size_t>(blockCount) * 16;
            std::atomic<bool> failed{};
            const std::size_t logicalCores = std::thread::hardware_concurrency();
            // This function already runs inside the low-priority cook task. Limit
            // BC7 workers so the editor and render driver retain CPU capacity.
            const std::size_t workerCount = std::min<std::size_t>(3, logicalCores > 2 ? logicalCores - 2 : 1);
            const auto encodeRange = [&](const std::uint32_t first, const std::uint32_t last) {
                std::array<std::uint8_t, 64> block{};
                for (std::uint32_t index = first; index < last; ++index) {
                    const std::uint32_t bx = index % blocksWide;
                    const std::uint32_t by = index / blocksWide;
                for (std::uint32_t y = 0; y < 4; ++y) for (std::uint32_t x = 0; x < 4; ++x) {
                    const auto sourceX = std::min(width - 1, bx * 4 + x);
                    const auto sourceY = std::min(height - 1, by * 4 + y);
                    const auto sourceOffset = (static_cast<std::size_t>(sourceY) * width + sourceX) * 4;
                    const auto blockOffset = (static_cast<std::size_t>(y) * 4 + x) * 4;
                    std::copy_n(rgba.data() + sourceOffset, 4, block.data() + blockOffset);
                }
                    if (CompressBlockBC7(block.data(), 16, output + static_cast<std::size_t>(index) * 16) != 0)
                        failed.store(true, std::memory_order_relaxed);
                }
            };
            std::vector<std::jthread> workers;
            workers.reserve(workerCount);
            for (std::size_t worker = 0; worker < workerCount; ++worker) {
                const auto first = static_cast<std::uint32_t>(static_cast<std::uint64_t>(blockCount) * worker / workerCount);
                const auto last = static_cast<std::uint32_t>(static_cast<std::uint64_t>(blockCount) * (worker + 1) / workerCount);
                workers.emplace_back(encodeRange, first, last);
            }
            // jthread destruction joins all workers before their output is used.
            workers.clear();
            if (failed.load(std::memory_order_relaxed))
                throw std::runtime_error("Compressonator could not encode a BC7 block");
        }

        [[nodiscard]] bool source_texture(const std::filesystem::path& path) {
            std::string extension = path.extension().string();
            std::ranges::transform(extension, extension.begin(), [](const unsigned char value) {
                return static_cast<char>(std::tolower(value));
            });
            return extension == ".png" || extension == ".jpg" || extension == ".jpeg" ||
                   extension == ".tga" || extension == ".bmp";
        }

        [[nodiscard]] bool linear_texture(const std::filesystem::path& path) {
            std::string name = path.stem().string();
            std::ranges::transform(name, name.begin(), [](const unsigned char value) {
                return static_cast<char>(std::tolower(value));
            });
            return name.contains("normal") || name.contains("_n") || name.contains("orm") ||
                   name.contains("rough") || name.contains("metal") || name.contains("ao") ||
                   name.contains("mask") || name.contains("opacity");
        }
    }

    CookedTexture cook_bc7(const std::span<const std::uint8_t> rgbaPixels, const std::uint32_t width,
                           const std::uint32_t height, const bool srgb) {
        if (width == 0 || height == 0 || width > std::numeric_limits<std::size_t>::max() / height / 4 ||
            rgbaPixels.size() != static_cast<std::size_t>(width) * height * 4)
            throw std::invalid_argument("Texture cooker requires a complete RGBA8 image");
        CookedTexture result;
        result.width = width; result.height = height;
        result.format = srgb ? TextureFormat::BC7_SRGB : TextureFormat::BC7_UNORM;
        std::vector<std::uint8_t> mip(rgbaPixels.begin(), rgbaPixels.end());
        std::uint32_t mipWidth = width;
        std::uint32_t mipHeight = height;
        while (true) {
            const auto offset = static_cast<std::uint64_t>(result.data.size());
            encode_mip_bc7(mip, mipWidth, mipHeight, result.data);
            result.mips.push_back({offset, static_cast<std::uint64_t>(result.data.size()) - offset, mipWidth, mipHeight});
            if (mipWidth == 1 && mipHeight == 1) break;
            mip = downsample_rgba(mip, mipWidth, mipHeight, srgb);
            mipWidth = std::max(1U, mipWidth / 2);
            mipHeight = std::max(1U, mipHeight / 2);
        }
        return result;
    }

    TextureCookSummary cook_all_textures(const std::filesystem::path& assetRoot, TextureCookProgress* progress) {
        TextureCookSummary summary;
        std::error_code error;
        for (std::filesystem::recursive_directory_iterator it{assetRoot,
                 std::filesystem::directory_options::skip_permission_denied, error}, end;
             it != end; it.increment(error)) {
            if (error) {
                summary.errors += error.message() + "\n";
                error.clear();
                ++summary.failed;
                continue;
            }
            if (!it->is_regular_file(error) || !source_texture(it->path())) continue;
            ++summary.discovered;
            if (progress != nullptr)
                progress->discovered.store(summary.discovered, std::memory_order_release);
            auto output = it->path();
            output.replace_extension(".gtex");
            const auto sourceTime = std::filesystem::last_write_time(it->path(), error);
            const auto outputTime = std::filesystem::last_write_time(output, error);
            if (!error && outputTime >= sourceTime) {
                ++summary.skipped;
                if (progress != nullptr)
                    progress->completed.fetch_add(1, std::memory_order_release);
                continue;
            }
            error.clear();
            int width{};
            int height{};
            int channels{};
            stbi_uc* pixels = stbi_load(it->path().string().c_str(), &width, &height, &channels, STBI_rgb_alpha);
            if (pixels == nullptr || width <= 0 || height <= 0) {
                summary.errors += it->path().string() + ": cannot decode image\n";
                stbi_image_free(pixels);
                ++summary.failed;
                if (progress != nullptr)
                    progress->completed.fetch_add(1, std::memory_order_release);
                continue;
            }
            try {
                const auto count = static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * STBI_rgb_alpha;
                const auto cooked = cook_bc7(std::span<const std::uint8_t>{pixels, count},
                                              static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height),
                                              !linear_texture(it->path()));
                if (!save_gtex(output, cooked)) throw std::runtime_error("could not write " + output.string());
                ++summary.cooked;
            } catch (const std::exception& exception) {
                summary.errors += it->path().string() + ": " + exception.what() + "\n";
                ++summary.failed;
            }
            stbi_image_free(pixels);
            if (progress != nullptr)
                progress->completed.fetch_add(1, std::memory_order_release);
        }
        return summary;
    }
}
