#pragma once

#include <array>
#include <cstdint>
#include <glm/mat4x4.hpp>

namespace Engine {
    // Describes receiver coverage, excluding temporal projection jitter. The
    // page neighbourhood already covers sub-pixel jitter between frames.
    struct VsmFeedbackKey final {
        glm::mat4 viewProjection{1.0F};
        std::array<glm::mat4, 7> clipMatrices{};
        std::uint64_t sceneRevision{};
        std::uint64_t settingsRevision{};
        std::uint64_t transformRevision{}, deformationRevision{}, windRevision{}, animationFrame{};
        std::uint32_t width{}, height{}, quality{};

        [[nodiscard]] bool operator==(const VsmFeedbackKey& other) const noexcept {
            return viewProjection == other.viewProjection && clipMatrices == other.clipMatrices &&
                sceneRevision == other.sceneRevision && settingsRevision == other.settingsRevision &&
                transformRevision == other.transformRevision && deformationRevision == other.deformationRevision &&
                windRevision == other.windRevision && animationFrame == other.animationFrame &&
                width == other.width && height == other.height && quality == other.quality;
        }
    };

    class VsmFeedbackCache final {
    public:
        [[nodiscard]] bool needsUpdate(const VsmFeedbackKey& key) const noexcept {
            return !valid_ || !(key_ == key);
        }
        void commit(const VsmFeedbackKey& key) noexcept { key_ = key; valid_ = true; }
    private:
        VsmFeedbackKey key_{};
        bool valid_{};
    };
}
