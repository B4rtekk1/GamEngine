#pragma once

#include "Engine/Renderer/Materials/PBRMaterial.h"

#include <cstdint>

namespace Engine {
    /**
     * Surface shader families supported by the built-in material contract.
     *
     * The first renderer iteration only compiles StandardPBR.  The enum is a
     * stable authoring boundary: adding a shader family later must also add a
     * compatible pipeline bin rather than binding a shader per entity.
     */
    enum class MaterialShader : std::uint8_t {
        StandardPBR,
        Unlit,
        Hologram,
        Water,
    };

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
