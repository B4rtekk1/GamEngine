#pragma once

#include "Engine/Math/Math.h"
#include "Engine/ECS/Entity.h"

#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Engine {
class Registry;

inline constexpr std::uint32_t Probe_Box = 1u << 0u;
inline constexpr std::uint32_t Probe_BoxProject = 1u << 1u;
inline constexpr std::uint32_t Probe_Enabled = 1u << 2u;

/** std430 record shared with Forward/forward_pbr.slang. */
struct alignas(16) GpuReflectionProbe final {
    glm::vec4 positionAndBlendDistance{};
    glm::vec4 extentsAndShape{};
    std::uint32_t textureIndex{};
    std::int32_t priority{};
    std::uint32_t flags{};
    std::uint32_t padding{};
};
static_assert(sizeof(GpuReflectionProbe) == 48);

/** Collects ECS probe volumes into a dense GPU table. Capture ownership is
 * intentionally separate from selection: until a probe is baked its texture
 * slot resolves to the renderer's prefiltered-environment fallback. */
class ReflectionProbeManager final {
public:
    static constexpr std::uint32_t MaxProbes = 256;
    // Kept equal to MaxProbes so an entry's textureIndex can be used directly
    // with the fixed-size descriptor array.  Slot zero is always a valid
    // prefiltered-sky fallback.
    static constexpr std::uint32_t TextureDescriptorCount = MaxProbes;
    void update(const Registry& registry);
    /** Queues an ECS probe for a renderer-owned cubemap capture. */
    void bakeProbe(Entity entity);
    /** Returns and clears requests; only the Vulkan backend may consume them. */
    [[nodiscard]] std::vector<Entity> consumeBakeRequests();
    /** Publishes the descriptor-array slot after the six faces were rendered. */
    void markBaked(Entity entity);
    void uploadProbeTable(std::uint32_t frameIndex) noexcept;
    [[nodiscard]] const std::vector<GpuReflectionProbe>& probes() const noexcept { return probes_; }

private:
    std::vector<GpuReflectionProbe> probes_;
    std::unordered_map<Entity, std::uint32_t> slots_;
    std::unordered_set<Entity> baked_;
    std::vector<Entity> bakeRequests_;
};
} // namespace Engine
