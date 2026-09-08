#pragma once

#include "Engine/Renderer/Materials/PBRMaterial.h"

#include <cstddef>
#include <cstdint>

namespace Engine {
    /**
     * Surface shader families supported by the built-in material contract.
     *
     * This is a stable authoring boundary. The renderer resolves it through
     * its ForwardPass pipeline registry; it is never a shader-file path.
     */
    enum class MaterialShader : std::uint8_t {
        StandardPBR,
        Unlit,
        Hologram,
        Water,
    };

    inline constexpr std::size_t MaterialShaderCount = 4;

    [[nodiscard]] constexpr std::size_t materialShaderIndex(const MaterialShader shader) noexcept {
        return static_cast<std::size_t>(shader);
    }

    /** Pipeline-relevant state authored with a surface material. */
    struct MaterialRenderState final {
        bool doubleSided{false};
        bool depthWrite{true};
        bool transparent{false};
    };

    /**
     * Author-facing surface material.
     *
     * PBRMaterial remains the parameter block consumed by the built-in PBR
     * shader.  It deliberately does not own shader or Vulkan pipeline state.
     */
    struct Material final {
        MaterialShader shader{MaterialShader::StandardPBR};
        PBRMaterial pbr{};
        MaterialRenderState renderState{};

        /** Keeps legacy PBR flags and explicit render state coherent. */
        void synchronizeRenderStateFromPbr() noexcept {
            renderState.doubleSided = pbr.doubleSided;
            renderState.transparent = pbr.alphaMode == AlphaMode::Blend;
        }
    };
} // namespace Engine
