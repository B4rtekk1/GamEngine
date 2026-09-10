#include "Engine/Assets/TextureCooker.h"
#include "Engine/Assets/Gtex.h"

#include <cmp_core.h>
#include <stb_image.h>

#include <algorithm>
#include <atomic>
#include <array>
#include <cmath>
#include <cctype>
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
        constexpr float bc7Quality = 0.10F;

        [[nodiscard]] std::size_t texture_cooker_worker_count() noexcept {
            const std::size_t cores = std::thread::hardware_concurrency();
            return cores > 1 ? cores - 1 : 1;
        }

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

            TextureCookerWorkerPool(const TextureCookerWorkerPool&) = delete;
            TextureCookerWorkerPool& operator=(const TextureCookerWorkerPool&) = delete;

            template<typename Function>
            void parallel_for(const std::size_t count, Function&& function) {
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
                const auto callback = std::make_shared<std::decay_t<Function>>(std::forward<Function>(function));

                {
                    std::scoped_lock lock(mutex_);
                    for (std::size_t task = 0; task < taskCount; ++task) {
                        tasks_.emplace_back([state, callback, count](void* options) {
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
            using Task = std::function<void(void*)>;

            void worker_main() {
                void* rawOptions = nullptr;
                if (CreateOptionsBC7(&rawOptions) != 0 || rawOptions == nullptr ||
                    SetQualityBC7(rawOptions, bc7Quality) != 0) {
                    if (rawOptions != nullptr) DestroyOptionsBC7(rawOptions);
                    rawOptions = nullptr;
                }
                const auto destroyOptions = [](void* options) { if (options != nullptr) DestroyOptionsBC7(options); };
                const std::unique_ptr<void, decltype(destroyOptions)> options{rawOptions, destroyOptions};

                for (;;) {
                    Task task;
                    {
                        std::unique_lock lock(mutex_);
                        available_.wait(lock, [this] { return stopping_ || !tasks_.empty(); });
                        if (stopping_ && tasks_.empty()) return;
                        task = std::move(tasks_.front());
                        tasks_.pop_front();
                    }
                    task(options.get());
                }
            }

            std::mutex mutex_;
            std::condition_variable available_;
            bool stopping_{};
            std::deque<Task> tasks_;
            std::vector<std::jthread> workers_;
        };

        [[nodiscard]] TextureCookerWorkerPool& texture_cooker_workers() {
            static TextureCookerWorkerPool pool;
            return pool;
        }

        [[nodiscard]] const std::array<float, 256>& srgb_to_linear_lut() noexcept {
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

        [[nodiscard]] const std::array<std::uint8_t, 65536>& linear_to_srgb_lut() noexcept {
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
            const auto& srgbToLinear = srgb_to_linear_lut();
            texture_cooker_workers().parallel_for(height, [&](void*, const std::size_t row) {
                const std::uint32_t y = static_cast<std::uint32_t>(row);
                for (std::uint32_t x = 0; x < width; ++x) {
                std::array<float, 4> sum{};
                for (std::uint32_t oy = 0; oy < 2; ++oy) for (std::uint32_t ox = 0; ox < 2; ++ox) {
                    const auto sx = std::min(sourceWidth - 1, x * 2 + ox);
                    const auto sy = std::min(sourceHeight - 1, y * 2 + oy);
                    const auto offset = (static_cast<std::size_t>(sy) * sourceWidth + sx) * 4;
                    for (std::uint32_t channel = 0; channel < 3; ++channel)
                        sum[channel] += srgb ? srgbToLinear[source[offset + channel]] : static_cast<float>(source[offset + channel]);
                    sum[3] += static_cast<float>(source[offset + 3]);
                }
                const auto output = (static_cast<std::size_t>(y) * width + x) * 4;
                for (std::uint32_t channel = 0; channel < 3; ++channel)
                    result[output + channel] = srgb ? linear_to_srgb(sum[channel] / 4.0F) : static_cast<std::uint8_t>(std::round(sum[channel] / 4.0F));
                result[output + 3] = static_cast<std::uint8_t>(std::round(sum[3] / 4.0F));
                }
            });
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
            std::atomic<std::uint32_t> nextBlock{};
            texture_cooker_workers().parallel_for(texture_cooker_worker_count(), [&](void* options, std::size_t) {
                if (options == nullptr) {
                    failed.store(true, std::memory_order_relaxed);
                    return;
                }
                std::array<std::uint8_t, 64> block{};
                constexpr std::uint32_t blocksPerChunk = 512;
                for (;;) {
                    const std::uint32_t first = nextBlock.fetch_add(blocksPerChunk, std::memory_order_relaxed);
                    if (first >= blockCount) break;
                    const std::uint32_t last = std::min(blockCount, first + blocksPerChunk);
                    for (std::uint32_t index = first; index < last; ++index) {
                        const std::uint32_t bx = index % blocksWide;
                        const std::uint32_t by = index / blocksWide;
                        const std::uint32_t px = bx * 4;
                        const std::uint32_t py = by * 4;
                        auto* destinationBlock = output + static_cast<std::size_t>(index) * 16;
                        if (px + 4 <= width && py + 4 <= height) {
                            const auto* sourceBlock = rgba.data() + (static_cast<std::size_t>(py) * width + px) * 4;
                            if (CompressBlockBC7(sourceBlock, width * 4, destinationBlock, options) != 0)
                                failed.store(true, std::memory_order_relaxed);
                            continue;
                        }
                        for (std::uint32_t y = 0; y < 4; ++y) for (std::uint32_t x = 0; x < 4; ++x) {
                            const auto sourceX = std::min(width - 1, px + x);
                            const auto sourceY = std::min(height - 1, py + y);
                            const auto sourceOffset = (static_cast<std::size_t>(sourceY) * width + sourceX) * 4;
                            const auto blockOffset = (static_cast<std::size_t>(y) * 4 + x) * 4;
                            std::copy_n(rgba.data() + sourceOffset, 4, block.data() + blockOffset);
                        }
                        if (CompressBlockBC7(block.data(), 16, destinationBlock, options) != 0)
                            failed.store(true, std::memory_order_relaxed);
                    }
                }
            });
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
        std::vector<std::filesystem::path> sources;
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
            sources.push_back(it->path());
        }
        summary.discovered = static_cast<std::uint32_t>(sources.size());
        if (progress != nullptr)
            progress->discovered.store(summary.discovered, std::memory_order_release);

        for (const auto& source : sources) {
            auto output = source;
            output.replace_extension(".gtex");
            const auto sourceTime = std::filesystem::last_write_time(source, error);
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
            stbi_uc* pixels = stbi_load(source.string().c_str(), &width, &height, &channels, STBI_rgb_alpha);
            if (pixels == nullptr || width <= 0 || height <= 0) {
                summary.errors += source.string() + ": cannot decode image\n";
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
                                              !linear_texture(source));
                if (!save_gtex(output, cooked)) throw std::runtime_error("could not write " + output.string());
                ++summary.cooked;
            } catch (const std::exception& exception) {
                summary.errors += source.string() + ": " + exception.what() + "\n";
                ++summary.failed;
            }
            stbi_image_free(pixels);
            if (progress != nullptr)
                progress->completed.fetch_add(1, std::memory_order_release);
        }
        return summary;
    }
}
