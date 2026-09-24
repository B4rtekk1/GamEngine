#include "Engine/Assets/TextureCooker.h"
#include "CookCache.h"
#include "Engine/Assets/Gtex.h"
#include "Engine/Assets/Gmesh.h"

#include <cmp_core.h>
#include <stb_image.h>

#include <algorithm>
#include <atomic>
#include <array>
#include <cmath>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

namespace Engine::Assets {
    namespace {
        constexpr float compressionQuality = CookCache::compressionQuality;

        struct TextureCookTimings final {
            std::chrono::nanoseconds mipGeneration{};
            std::chrono::nanoseconds blockEncode{};
        };

        [[nodiscard]] double milliseconds(const std::chrono::nanoseconds duration) noexcept {
            return std::chrono::duration<double, std::milli>(duration).count();
        }

        [[nodiscard]] std::size_t texture_cooker_worker_count() noexcept {
            const std::size_t cores = std::thread::hardware_concurrency();
            return cores > 1 ? cores - 1 : 1;
        }

        struct WorkerOptions final {
            void *bc4{};
            void *bc5{};
            void *bc7{};
            ~WorkerOptions() {
                if (bc4 != nullptr) DestroyOptionsBC4(bc4);
                if (bc5 != nullptr) DestroyOptionsBC5(bc5);
                if (bc7 != nullptr) DestroyOptionsBC7(bc7);
            }
        };

        class TextureCookerWorkerPool final {
        public:
            TextureCookerWorkerPool() {
                const std::size_t workerCount = texture_cooker_worker_count();
                workers_.reserve(workerCount);
                for (std::size_t worker = 0; worker < workerCount; ++worker)
                    workers_.emplace_back([this] { worker_main(); });
            }

            ~TextureCookerWorkerPool() {
                {
                    std::scoped_lock lock(mutex_);
                    stopping_ = true;
                }
                available_.notify_all();
            }

            TextureCookerWorkerPool(const TextureCookerWorkerPool &) = delete;

            TextureCookerWorkerPool &operator=(const TextureCookerWorkerPool &) = delete;

            template<typename Function>
            void parallel_for(const std::size_t count, Function &&function) {
                if (count == 0) return;

                struct State final {
                    std::atomic<std::size_t> next{};
                    std::atomic<std::size_t> remaining{};
                    std::mutex mutex;
                    std::condition_variable completed;
                };
                const auto state = std::make_shared<State>();
                const std::size_t taskCount = std::min(count, workers_.size());
                state->remaining.store(taskCount, std::memory_order_relaxed);
                const auto callback = std::make_shared<std::decay_t<Function> >(std::forward<Function>(function));

                {
                    std::scoped_lock lock(mutex_);
                    for (std::size_t task = 0; task < taskCount; ++task) {
                        tasks_.emplace_back([state, callback, count](void *options) {
                            for (;;) {
                                const std::size_t index = state->next.fetch_add(1, std::memory_order_relaxed);
                                if (index >= count) break;
                                (*callback)(options, index);
                            }
                            if (state->remaining.fetch_sub(1, std::memory_order_acq_rel) == 1)
                                state->completed.notify_one();
                        });
                    }
                }
                available_.notify_all();

                std::unique_lock lock(state->mutex);
                state->completed.wait(lock, [&] { return state->remaining.load(std::memory_order_acquire) == 0; });
            }

        private:
            using Task = std::function<void(void *)>;

            void worker_main() {
                WorkerOptions options;
                if (CreateOptionsBC4(&options.bc4) != 0 || options.bc4 == nullptr ||
                    SetQualityBC4(options.bc4, compressionQuality) != 0) {
                    if (options.bc4 != nullptr) DestroyOptionsBC4(options.bc4);
                    options.bc4 = nullptr;
                }
                if (CreateOptionsBC5(&options.bc5) != 0 || options.bc5 == nullptr ||
                    SetQualityBC5(options.bc5, compressionQuality) != 0) {
                    if (options.bc5 != nullptr) DestroyOptionsBC5(options.bc5);
                    options.bc5 = nullptr;
                }
                if (CreateOptionsBC7(&options.bc7) != 0 || options.bc7 == nullptr ||
                    SetQualityBC7(options.bc7, compressionQuality) != 0) {
                    if (options.bc7 != nullptr) DestroyOptionsBC7(options.bc7);
                    options.bc7 = nullptr;
                }

                for (;;) {
                    Task task;
                    {
                        std::unique_lock lock(mutex_);
                        available_.wait(lock, [this] { return stopping_ || !tasks_.empty(); });
                        if (stopping_ && tasks_.empty()) return;
                        task = std::move(tasks_.front());
                        tasks_.pop_front();
                    }
                    task(&options);
                }
            }

            std::mutex mutex_;
            std::condition_variable available_;
            bool stopping_{};
            std::deque<Task> tasks_;
            std::vector<std::jthread> workers_;
        };

        [[nodiscard]] TextureCookerWorkerPool &texture_cooker_workers() {
            static TextureCookerWorkerPool pool;
            return pool;
        }

        [[nodiscard]] const std::array<float, 256> &srgb_to_linear_lut() noexcept {
            static const std::array<float, 256> lut = [] {
                std::array<float, 256> result{};
                for (std::uint32_t value = 0; value < result.size(); ++value) {
                    const float encoded = static_cast<float>(value) / 255.0F;
                    result[value] = encoded <= 0.04045F
                                        ? encoded / 12.92F
                                        : std::pow((encoded + 0.055F) / 1.055F, 2.4F);
                }
                return result;
            }();
            return lut;
        }

        [[nodiscard]] const std::array<std::uint8_t, 65536> &linear_to_srgb_lut() noexcept {
            static const std::array<std::uint8_t, 65536> lut = [] {
                std::array<std::uint8_t, 65536> result{};
                for (std::uint32_t index = 0; index < result.size(); ++index) {
                    const float linear = static_cast<float>(index) / 65535.0F;
                    const float encoded = linear <= 0.0031308F
                                              ? linear * 12.92F
                                              : 1.055F * std::pow(linear, 1.0F / 2.4F) - 0.055F;
                    result[index] = static_cast<std::uint8_t>(std::round(encoded * 255.0F));
                }
                return result;
            }();
            return lut;
        }

        [[nodiscard]] std::uint8_t linear_to_srgb(const float value) noexcept {
            const float linear = std::clamp(value, 0.0F, 1.0F);
            const std::uint32_t index = std::min(static_cast<std::uint32_t>(linear * 65535.0F), 65535U);
            return linear_to_srgb_lut()[index];
        }

        [[nodiscard]] std::vector<std::uint8_t> downsample_rgba(const std::span<const std::uint8_t> source,
                                                                const std::uint32_t sourceWidth,
                                                                const std::uint32_t sourceHeight,
                                                                const bool srgb) {
            const std::uint32_t width = std::max(1U, sourceWidth / 2);
            const std::uint32_t height = std::max(1U, sourceHeight / 2);
            std::vector<std::uint8_t> result(static_cast<std::size_t>(width) * height * 4);
            const auto &srgbToLinear = srgb_to_linear_lut();
            texture_cooker_workers().parallel_for(height, [&](void *, const std::size_t row) {
                const std::uint32_t y = static_cast<std::uint32_t>(row);
                for (std::uint32_t x = 0; x < width; ++x) {
                    std::array<float, 4> sum{};
                    for (std::uint32_t oy = 0; oy < 2; ++oy)
                        for (std::uint32_t ox = 0; ox < 2; ++ox) {
                            const auto sx = std::min(sourceWidth - 1, x * 2 + ox);
                            const auto sy = std::min(sourceHeight - 1, y * 2 + oy);
                            const auto offset = (static_cast<std::size_t>(sy) * sourceWidth + sx) * 4;
                            for (std::uint32_t channel = 0; channel < 3; ++channel)
                                sum[channel] += srgb
                                                    ? srgbToLinear[source[offset + channel]]
                                                    : static_cast<float>(source[offset + channel]);
                            sum[3] += static_cast<float>(source[offset + 3]);
                        }
                    const auto output = (static_cast<std::size_t>(y) * width + x) * 4;
                    for (std::uint32_t channel = 0; channel < 3; ++channel)
                        result[output + channel] = srgb
                                                       ? linear_to_srgb(sum[channel] / 4.0F)
                                                       : static_cast<std::uint8_t>(std::round(sum[channel] / 4.0F));
                    result[output + 3] = static_cast<std::uint8_t>(std::round(sum[3] / 4.0F));
                }
            });
            return result;
        }

        [[nodiscard]] std::vector<std::uint8_t> downsample_normal_map(const std::span<const std::uint8_t> source,
                                                                      const std::uint32_t sourceWidth,
                                                                      const std::uint32_t sourceHeight) {
            const auto width = std::max(1U, sourceWidth / 2);
            const auto height = std::max(1U, sourceHeight / 2);
            std::vector<std::uint8_t> result(static_cast<std::size_t>(width) * height * 4);
            texture_cooker_workers().parallel_for(height, [&](void *, const std::size_t row) {
                for (std::uint32_t x = 0; x < width; ++x) {
                    std::array<float, 3> sum{};
                    for (std::uint32_t oy = 0; oy < 2; ++oy)
                        for (std::uint32_t ox = 0; ox < 2; ++ox) {
                            const auto sx = std::min(sourceWidth - 1, x * 2 + ox);
                            const auto sy = std::min(sourceHeight - 1, static_cast<std::uint32_t>(row) * 2 + oy);
                            const auto offset = (static_cast<std::size_t>(sy) * sourceWidth + sx) * 4;
                            const auto nx = static_cast<float>(source[offset]) / 127.5F - 1.0F;
                            const auto ny = static_cast<float>(source[offset + 1]) / 127.5F - 1.0F;
                            sum[0] += nx;
                            sum[1] += ny;
                            sum[2] += std::sqrt(std::max(0.0F, 1.0F - nx * nx - ny * ny));
                        }
                    const auto length = std::sqrt(sum[0] * sum[0] + sum[1] * sum[1] + sum[2] * sum[2]);
                    if (length > 1e-6F)
                        for (float &component: sum) component /= length;
                    else sum = {0.0F, 0.0F, 1.0F};
                    const auto output = (row * width + x) * 4;
                    for (std::uint32_t channel = 0; channel < 3; ++channel)
                        result[output + channel] = static_cast<std::uint8_t>(std::round(
                            std::clamp(sum[channel] * 0.5F + 0.5F, 0.0F, 1.0F) * 255.0F));
                    result[output + 3] = 255;
                }
            });
            return result;
        }

        void encode_mip_bc7(const std::span<const std::uint8_t> rgba, const std::uint32_t width,
                            const std::uint32_t height, std::vector<std::uint8_t> &destination) {
            const std::uint32_t blocksWide = (width + 3) / 4;
            const std::uint32_t blocksHigh = (height + 3) / 4;
            const std::uint32_t blockCount = blocksWide * blocksHigh;
            destination.resize(destination.size() + static_cast<std::size_t>(blockCount) * 16);
            const auto output = destination.data() + destination.size() - static_cast<std::size_t>(blockCount) * 16;
            std::atomic<bool> failed{};
            std::atomic<std::uint32_t> nextBlock{};
            texture_cooker_workers().parallel_for(texture_cooker_worker_count(), [&](void *rawOptions, std::size_t) {
                const auto *options = static_cast<const WorkerOptions *>(rawOptions);
                if (options == nullptr || options->bc7 == nullptr) {
                    failed.store(true, std::memory_order_relaxed);
                    return;
                }
                std::array<std::uint8_t, 64> block{};
                for (;;) {
                    constexpr std::uint32_t blocksPerChunk = 512;
                    const std::uint32_t first = nextBlock.fetch_add(blocksPerChunk, std::memory_order_relaxed);
                    if (first >= blockCount) break;
                    const std::uint32_t last = std::min(blockCount, first + blocksPerChunk);
                    for (std::uint32_t index = first; index < last; ++index) {
                        const std::uint32_t bx = index % blocksWide;
                        const std::uint32_t by = index / blocksWide;
                        const std::uint32_t px = bx * 4;
                        const std::uint32_t py = by * 4;
                        auto *destinationBlock = output + static_cast<std::size_t>(index) * 16;
                        if (px + 4 <= width && py + 4 <= height) {
                            const auto *sourceBlock = rgba.data() + (static_cast<std::size_t>(py) * width + px) * 4;
                            if (CompressBlockBC7(sourceBlock, width * 4, destinationBlock, options->bc7) != 0)
                                failed.store(true, std::memory_order_relaxed);
                            continue;
                        }
                        for (std::uint32_t y = 0; y < 4; ++y)
                            for (std::uint32_t x = 0; x < 4; ++x) {
                                const auto sourceX = std::min(width - 1, px + x);
                                const auto sourceY = std::min(height - 1, py + y);
                                const auto sourceOffset = (static_cast<std::size_t>(sourceY) * width + sourceX) * 4;
                                const auto blockOffset = (static_cast<std::size_t>(y) * 4 + x) * 4;
                                std::copy_n(rgba.data() + sourceOffset, 4, block.data() + blockOffset);
                            }
                        if (CompressBlockBC7(block.data(), 16, destinationBlock, options->bc7) != 0)
                            failed.store(true, std::memory_order_relaxed);
                    }
                }
            });
            if (failed.load(std::memory_order_relaxed))
                throw std::runtime_error("Compressonator could not encode a BC7 block");
        }

        void encode_mip_bc4_bc5(const std::span<const std::uint8_t> rgba, const std::uint32_t width,
                                const std::uint32_t height, const TextureFormat format,
                                std::vector<std::uint8_t> &destination) {
            const auto blocksWide = (width + 3) / 4;
            const auto blocksHigh = (height + 3) / 4;
            const auto blockCount = blocksWide * blocksHigh;
            const auto bytesPerBlock = format == TextureFormat::BC4_UNORM ? 8U : 16U;
            const auto offset = destination.size();
            destination.resize(offset + static_cast<std::size_t>(blockCount) * bytesPerBlock);
            std::atomic<bool> failed{};
            std::atomic<std::uint32_t> nextBlock{};
            texture_cooker_workers().parallel_for(texture_cooker_worker_count(), [&](void *rawOptions, std::size_t) {
                const auto *options = static_cast<const WorkerOptions *>(rawOptions);
                if (options == nullptr || (format == TextureFormat::BC4_UNORM ? options->bc4 : options->bc5)
                    == nullptr) {
                    failed.store(true, std::memory_order_relaxed);
                    return;
                }
                std::array<std::uint8_t, 16> red{};
                std::array<std::uint8_t, 16> green{};
                for (;;) {
                    constexpr std::uint32_t blocksPerChunk = 512;
                    const auto first = nextBlock.fetch_add(blocksPerChunk, std::memory_order_relaxed);
                    if (first >= blockCount) break;
                    const auto last = std::min(blockCount, first + blocksPerChunk);
                    for (auto index = first; index < last; ++index) {
                        const auto px = (index % blocksWide) * 4;
                        const auto py = (index / blocksWide) * 4;
                        for (std::uint32_t y = 0; y < 4; ++y)
                            for (std::uint32_t x = 0; x < 4; ++x) {
                                const auto sx = std::min(width - 1, px + x);
                                const auto sy = std::min(height - 1, py + y);
                                const auto sourceOffset = (static_cast<std::size_t>(sy) * width + sx) * 4;
                                red[y * 4 + x] = rgba[sourceOffset];
                                green[y * 4 + x] = rgba[sourceOffset + 1];
                            }
                        auto *block = destination.data() + offset + static_cast<std::size_t>(index) * bytesPerBlock;
                        const auto status = format == TextureFormat::BC4_UNORM
                                                ? CompressBlockBC4(red.data(), 4, block, options->bc4)
                                                : CompressBlockBC5(red.data(), 4, green.data(), 4, block, options->bc5);
                        if (status != 0) failed.store(true, std::memory_order_relaxed);
                    }
                }
            });
            if (failed.load(std::memory_order_relaxed))
                throw std::runtime_error("Compressonator could not encode a BC4/BC5 block");
        }

        [[nodiscard]] bool source_texture(const std::filesystem::path &path) {
            std::string extension = path.extension().string();
            std::ranges::transform(extension, extension.begin(), [](const unsigned char value) {
                return static_cast<char>(std::tolower(value));
            });
            return extension == ".png" || extension == ".jpg" || extension == ".jpeg" ||
                   extension == ".tga" || extension == ".bmp";
        }

        [[nodiscard]] TextureFormat source_texture_format(const std::filesystem::path &path) {
            std::string name = path.stem().string();
            std::ranges::transform(name, name.begin(), [](const unsigned char value) {
                return static_cast<char>(std::tolower(value));
            });
            if (name.contains("normal") || name.ends_with("_n") || name.contains("_n_"))
                return TextureFormat::BC5_UNORM;
            if (name.contains("occlusion") || name == "ao" || name.starts_with("ao_") ||
                name.ends_with("_ao") || name.contains("_ao_") || name.contains("height") ||
                name.contains("displace") || name.contains("_disp") || name.contains("opacity") ||
                name.contains("mask"))
                return TextureFormat::BC4_UNORM;
            if (name.contains("orm") || name.contains("rough") || name.contains("metal") ||
                name.contains("specular")) return TextureFormat::BC7_UNORM;
            return TextureFormat::BC7_SRGB;
        }
    }

    [[nodiscard]] CookedTexture cook_texture_impl(const std::span<const std::uint8_t> rgbaPixels,
                                                  const std::uint32_t width, const std::uint32_t height,
                                                  const TextureFormat format, TextureCookTimings *const timings) {
        if (width == 0 || height == 0 || width > std::numeric_limits<std::size_t>::max() / height / 4 ||
            rgbaPixels.size() != static_cast<std::size_t>(width) * height * 4)
            throw std::invalid_argument("Texture cooker requires a complete RGBA8 image");
        if (format != TextureFormat::BC4_UNORM && format != TextureFormat::BC5_UNORM &&
            format != TextureFormat::BC7_UNORM && format != TextureFormat::BC7_SRGB)
            throw std::invalid_argument("Texture cooker requires a BC4, BC5, or BC7 format");
        CookedTexture result;
        result.width = width;
        result.height = height;
        result.format = format;
        const std::size_t bytesPerBlock = format == TextureFormat::BC4_UNORM ? 8 : 16;
        std::size_t totalBytes = 0;
        std::size_t mipCount = 0;
        for (auto w = width, h = height;;) {
            const auto blocksWide = (static_cast<std::size_t>(w) + 3) / 4;
            const auto blocksHigh = (static_cast<std::size_t>(h) + 3) / 4;
            if (blocksWide > std::numeric_limits<std::size_t>::max() / blocksHigh / bytesPerBlock ||
                totalBytes > std::numeric_limits<std::size_t>::max() - blocksWide * blocksHigh * bytesPerBlock)
                throw std::length_error("Compressed mip chain is too large");
            totalBytes += blocksWide * blocksHigh * bytesPerBlock;
            ++mipCount;
            if (w == 1 && h == 1) break;
            w = std::max(1U, w / 2);
            h = std::max(1U, h / 2);
        }
        result.data.reserve(totalBytes);
        result.mips.reserve(mipCount);
        std::span<const std::uint8_t> mip = rgbaPixels;
        std::vector<std::uint8_t> ownedMip;
        std::uint32_t mipWidth = width;
        std::uint32_t mipHeight = height;
        while (true) {
            const auto offset = static_cast<std::uint64_t>(result.data.size());
            const auto encodeStarted = std::chrono::steady_clock::now();
            if (format == TextureFormat::BC4_UNORM || format == TextureFormat::BC5_UNORM)
                encode_mip_bc4_bc5(mip, mipWidth, mipHeight, format, result.data);
            else encode_mip_bc7(mip, mipWidth, mipHeight, result.data);
            if (timings != nullptr)
                timings->blockEncode += std::chrono::steady_clock::now() - encodeStarted;
            result.mips.push_back(
                {offset, static_cast<std::uint64_t>(result.data.size()) - offset, mipWidth, mipHeight});
            if (mipWidth == 1 && mipHeight == 1) break;
            const auto mipStarted = std::chrono::steady_clock::now();
            ownedMip = format == TextureFormat::BC5_UNORM
                           ? downsample_normal_map(mip, mipWidth, mipHeight)
                           : downsample_rgba(mip, mipWidth, mipHeight, format == TextureFormat::BC7_SRGB);
            mip = ownedMip;
            if (timings != nullptr)
                timings->mipGeneration += std::chrono::steady_clock::now() - mipStarted;
            mipWidth = std::max(1U, mipWidth / 2);
            mipHeight = std::max(1U, mipHeight / 2);
        }
        return result;
    }

    CookedTexture cook_bc7(const std::span<const std::uint8_t> rgbaPixels, const std::uint32_t width,
                           const std::uint32_t height, const bool srgb) {
        return cook_texture_impl(rgbaPixels, width, height,
                                 srgb ? TextureFormat::BC7_SRGB : TextureFormat::BC7_UNORM, nullptr);
    }

    CookedTexture cook_texture(const std::span<const std::uint8_t> rgbaPixels, const std::uint32_t width,
                               const std::uint32_t height, const TextureFormat format) {
        return cook_texture_impl(rgbaPixels, width, height, format, nullptr);
    }

    TextureFormat default_texture_format(const std::filesystem::path &source) {
        return source_texture_format(source);
    }

    bool current_source_texture(const std::filesystem::path &source,
                                const std::filesystem::path &output, const TextureFormat format) {
        const auto sourceHash = CookCache::hash_file(source);
        if (!sourceHash) return false;
        const auto key = CookCache::texture_key({.sourceHash = *sourceHash, .format = format,
                                                .srgb = format == TextureFormat::BC7_SRGB});
        if (!CookCache::artifact_has_key(output, key)) return false;
        const auto cooked = load_gtex(output);
        return cooked && cooked->format == format;
    }

    namespace {
        [[nodiscard]] TextureCookResult resolve_cached_texture(const std::uint64_t sourceHash,
                                                                const std::filesystem::path &output,
                                                                const TextureFormat format,
                                                                const std::filesystem::path &cacheRoot,
                                                                const std::function<CookedTexture()> &encode,
                                                                TexturePhaseTimings *const timings) {
            const CookCache::TextureCookKey settings{
                .sourceHash = sourceHash,
                .format = format,
                .srgb = format == TextureFormat::BC7_SRGB,
            };
            const auto key = CookCache::texture_key(settings);
            const auto cached = CookCache::texture_path(cacheRoot, key);
            const auto valid = [&](const std::filesystem::path &candidate) {
                const auto texture = load_gtex(candidate);
                return texture && texture->format == settings.format;
            };
            if (CookCache::artifact_has_key(output, key) && valid(output)) {
                if (!valid(cached)) {
                    const auto writeStarted = std::chrono::steady_clock::now();
                    std::error_code error;
                    std::filesystem::create_directories(cached.parent_path(), error);
                    if (!error) std::filesystem::copy_file(output, cached,
                        std::filesystem::copy_options::overwrite_existing, error);
                    if (timings != nullptr)
                        timings->writeMilliseconds += milliseconds(std::chrono::steady_clock::now() - writeStarted);
                }
                return TextureCookResult::Reused;
            }
            if (valid(cached)) {
                const auto writeStarted = std::chrono::steady_clock::now();
                const auto published = CookCache::publish(cached, output, key);
                if (timings != nullptr)
                    timings->writeMilliseconds += milliseconds(std::chrono::steady_clock::now() - writeStarted);
                return published ? TextureCookResult::Reused : TextureCookResult::Failed;
            }
            try {
                const auto cooked = encode();
                const auto writeStarted = std::chrono::steady_clock::now();
                std::error_code error;
                std::filesystem::create_directories(cached.parent_path(), error);
                if (error || !save_gtex(cached, cooked)) return TextureCookResult::Failed;
                if (!CookCache::publish(cached, output, key)) return TextureCookResult::Failed;
                if (timings != nullptr)
                    timings->writeMilliseconds += milliseconds(std::chrono::steady_clock::now() - writeStarted);
                return TextureCookResult::Cooked;
            } catch (const std::exception &) {
                return TextureCookResult::Failed;
            }
        }
    }

    TextureCookResult cook_source_texture_cached(const std::filesystem::path &source,
                                                 const std::filesystem::path &output, const TextureFormat format,
                                                 const std::filesystem::path &cacheRoot,
                                                 TexturePhaseTimings *const timings) {
        const auto hashStarted = std::chrono::steady_clock::now();
        const auto sourceHash = CookCache::hash_file(source);
        if (timings != nullptr)
            timings->sourceHashMilliseconds += milliseconds(std::chrono::steady_clock::now() - hashStarted);
        if (!sourceHash) return TextureCookResult::Failed;
        return resolve_cached_texture(*sourceHash, output, format, cacheRoot, [&] {
            int width = 0, height = 0, channels = 0;
            const auto decodeStarted = std::chrono::steady_clock::now();
            stbi_uc *pixels = stbi_load(source.string().c_str(), &width, &height, &channels, STBI_rgb_alpha);
            if (timings != nullptr)
                timings->decodeMilliseconds += milliseconds(std::chrono::steady_clock::now() - decodeStarted);
            if (pixels == nullptr || width <= 0 || height <= 0) {
                stbi_image_free(pixels);
                throw std::runtime_error("cannot decode source image");
            }
            const auto count = static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * STBI_rgb_alpha;
            TextureCookTimings cookTimings;
            try {
                auto cooked = cook_texture_impl(std::span<const std::uint8_t>{pixels, count},
                                                static_cast<std::uint32_t>(width),
                                                static_cast<std::uint32_t>(height), format, &cookTimings);
                stbi_image_free(pixels);
                if (timings != nullptr) {
                    timings->mipGenerationMilliseconds += milliseconds(cookTimings.mipGeneration);
                    timings->blockEncodeMilliseconds += milliseconds(cookTimings.blockEncode);
                }
                return cooked;
            } catch (...) {
                stbi_image_free(pixels);
                throw;
            }
        }, timings);
    }

    TextureCookResult cook_image_texture_cached(const std::span<const std::uint8_t> rgbaPixels,
                                                const std::uint32_t width, const std::uint32_t height,
                                                const std::filesystem::path &output, const TextureFormat format,
                                                const std::filesystem::path &cacheRoot,
                                                TexturePhaseTimings *const timings) {
        CookCache::Hash64 sourceHash;
        sourceHash.add(rgbaPixels);
        sourceHash.add(width);
        sourceHash.add(height);
        return resolve_cached_texture(sourceHash.value, output, format, cacheRoot, [&] {
            TextureCookTimings cookTimings;
            auto cooked = cook_texture_impl(rgbaPixels, width, height, format, &cookTimings);
            if (timings != nullptr) {
                timings->mipGenerationMilliseconds += milliseconds(cookTimings.mipGeneration);
                timings->blockEncodeMilliseconds += milliseconds(cookTimings.blockEncode);
            }
            return cooked;
        }, timings);
    }

    TextureCookSummary cook_all_textures(const std::filesystem::path &assetRoot, TextureCookProgress *progress) {
        TextureCookSummary summary;
        std::error_code error;
        std::vector<std::filesystem::path> sources;
        for (std::filesystem::recursive_directory_iterator it{
                         assetRoot,
                         std::filesystem::directory_options::skip_permission_denied, error
                     }, end;
             it != end; it.increment(error)) {
            if (error) {
                summary.errors += error.message() + "\n";
                error.clear();
                ++summary.failed;
                continue;
            }
            if (!it->is_regular_file(error) || !source_texture(it->path())) continue;
            sources.push_back(it->path());
        }
        summary.discovered = static_cast<std::uint32_t>(sources.size());
        if (progress != nullptr)
            progress->discovered.store(summary.discovered, std::memory_order_release);

        for (const auto &source: sources) {
            auto output = source;
            output.replace_extension(".gtex");
            switch (cook_source_texture_cached(source, output, default_texture_format(source),
                                               assetRoot.parent_path() / "Library" / "DDC",
                                               &summary.standalone)) {
                case TextureCookResult::Reused: ++summary.skipped; break;
                case TextureCookResult::Cooked: ++summary.cooked; break;
                case TextureCookResult::Failed:
                    ++summary.failed;
                    summary.errors += source.string() + ": could not cook texture\n";
                    break;
            }
            if (progress != nullptr)
                progress->completed.fetch_add(1, std::memory_order_release);
        }
        return summary;
    }

    TextureCookSummary cook_all_gltf_meshes(const std::filesystem::path &assetRoot, TextureCookProgress *progress) {
        TextureCookSummary summary;
        std::error_code error;
        std::vector<std::filesystem::path> sources;
        for (std::filesystem::recursive_directory_iterator it{
                         assetRoot,
                         std::filesystem::directory_options::skip_permission_denied, error
                     }, end;
             it != end; it.increment(error)) {
            if (error) {
                summary.errors += error.message() + "\n";
                error.clear();
                ++summary.failed;
                continue;
            }
            if (!it->is_regular_file(error)) continue;
            auto extension = it->path().extension().string();
            std::ranges::transform(extension, extension.begin(), [](const unsigned char value) {
                return static_cast<char>(std::tolower(value));
            });
            if (extension == ".glb" || extension == ".gltf") sources.push_back(it->path());
        }
        summary.discovered = static_cast<std::uint32_t>(sources.size());
        if (progress != nullptr) progress->discovered.store(summary.discovered, std::memory_order_release);
        for (const auto &source: sources) {
            auto cooked = source;
            cooked.replace_extension(".gmesh");
            const auto scanStarted = std::chrono::steady_clock::now();
            const auto current = current_gltf_mesh(source, cooked);
            summary.gltf.sourceScanMilliseconds += milliseconds(std::chrono::steady_clock::now() - scanStarted);
            if (current) ++summary.skipped;
            else if (cook_gltf_mesh(source, assetRoot.parent_path() / "Library" / "DDC", &summary)) ++summary.cooked;
            else {
                ++summary.failed;
                summary.errors += source.string() + ": could not cook glTF mesh\n";
            }
            if (progress != nullptr) progress->completed.fetch_add(1, std::memory_order_release);
        }
        return summary;
    }
}
