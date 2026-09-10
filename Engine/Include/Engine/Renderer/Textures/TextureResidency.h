#pragma once

#include "Engine/Assets/AssetTypes.h"

#include <algorithm>
#include <cstdint>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Engine {
    /**
     * Chooses the first mip that is physically present in each streamed image.
     * A lower number means sharper texture.  Call consumeChanges() on the render
     * thread and rebuild the corresponding Texture2D with createGtex().
     */
    class TextureResidencyManager final {
    public:
        struct Change { std::uint64_t id; std::uint32_t firstResidentMip; };

        explicit TextureResidencyManager(std::uint64_t budgetBytes = 512ULL * 1024 * 1024)
            : budgetBytes_(budgetBytes) {}

        void setBudget(std::uint64_t bytes) noexcept { budgetBytes_ = bytes; }
        [[nodiscard]] std::uint64_t budget() const noexcept { return budgetBytes_; }
        [[nodiscard]] std::uint64_t residentBytes() const noexcept { return residentBytes_; }

        void registerTexture(std::uint64_t id, const Assets::GtexTexture& texture) {
            Entry entry{&texture, static_cast<std::uint32_t>(texture.mips.size() - 1),
                        static_cast<std::uint32_t>(texture.mips.size() - 1)};
            entries_.insert_or_assign(id, entry);
            recalculateBytes();
        }

        /** desiredMip comes from projected texel density; priority breaks budget ties. */
        void request(std::uint64_t id, std::uint32_t desiredMip, float priority) {
            if (auto it = entries_.find(id); it != entries_.end()) {
                Entry& entry = it->second;
                const auto wanted = std::min(desiredMip, static_cast<std::uint32_t>(entry.texture->mips.size() - 1));
                if (wanted != entry.wantedMip) {
                    entry.wantedMip = wanted;
                    entry.lastRequestedFrame = frame_;
                }
                entry.priority = std::max(0.0F, priority);
            }
        }

        /** Applies budget, priority and a frame hysteresis before emitting GPU rebuilds. */
        void update() {
            ++frame_;
            std::vector<std::pair<std::uint64_t, Entry*>> ordered;
            for (auto& [id, entry] : entries_) ordered.emplace_back(id, &entry);
            std::sort(ordered.begin(), ordered.end(), [](const auto& a, const auto& b) {
                return a.second->priority > b.second->priority;
            });
            std::uint64_t used = 0;
            for (const auto& [id, entry] : ordered) {
                std::uint32_t target = entry->wantedMip;
                // Avoid flapping: promotion needs two consistent frames; eviction is immediate under pressure.
                if (target < entry->residentMip && frame_ - entry->lastRequestedFrame < kPromotionHysteresisFrames)
                    target = entry->residentMip;
                while (target + 1 < entry->texture->mips.size() && used + bytesFrom(*entry->texture, target) > budgetBytes_)
                    ++target;
                if (target != entry->residentMip) {
                    entry->residentMip = target;
                    changes_.push_back({id, target});
                }
                used += bytesFrom(*entry->texture, entry->residentMip);
            }
            residentBytes_ = used;
        }

        [[nodiscard]] std::vector<Change> consumeChanges() { return std::exchange(changes_, {}); }

    private:
        static constexpr std::uint64_t kPromotionHysteresisFrames = 2;
        struct Entry { const Assets::GtexTexture* texture; std::uint32_t residentMip; std::uint32_t wantedMip; float priority{}; std::uint64_t lastRequestedFrame{}; };
        static std::uint64_t bytesFrom(const Assets::GtexTexture& texture, std::uint32_t firstMip) {
            std::uint64_t total{};
            for (std::uint32_t mip = firstMip; mip < texture.mips.size(); ++mip) total += texture.mips[mip].size;
            return total;
        }
        void recalculateBytes() { residentBytes_ = 0; for (const auto& [id, entry] : entries_) residentBytes_ += bytesFrom(*entry.texture, entry.residentMip); }
        std::unordered_map<std::uint64_t, Entry> entries_;
        std::vector<Change> changes_;
        std::uint64_t budgetBytes_{};
        std::uint64_t residentBytes_{};
        std::uint64_t frame_{};
    };
}
