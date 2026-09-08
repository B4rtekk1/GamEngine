#pragma once

#include "Engine/Math/Vec2.h"
#include "Engine/Math/Vec3.h"
#include "Engine/Math/Vec4.h"
#include "Engine/Scene/Components/IdentityComponents.h"

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace Engine {
    /** Values which may travel over a shader-graph pin. */
    enum class ShaderValueType : std::uint8_t {
        Float, Float2, Float3, Float4, Bool, Texture2D
    };

    /** The deliberately small, surface-only MVP node set. */
    enum class ShaderNodeType : std::uint8_t {
        Float, Vector2, Vector3, Vector4, Property,
        Add, Subtract, Multiply, Divide,
        Lerp, Clamp, Saturate, OneMinus,
        Texture2D, SampleTexture2D,
        UV, Time, Normal, ViewDirection, Fresnel,
        SurfaceOutput
    };

    using ShaderNodeId = std::uint32_t;
    using ShaderPinId = std::uint32_t;
    using ShaderProgramId = std::uint64_t;

    /** A pin id is global within one graph. */
    struct ShaderPin final {
        ShaderPinId id{};
        std::string name;
        ShaderValueType type{ShaderValueType::Float};
    };

    using ShaderNodeValue = std::variant<std::monostate, float, Vec2, Vec3, Vec4, std::string>;

    struct ShaderNode final {
        ShaderNodeId id{};
        ShaderNodeType type{ShaderNodeType::Float};
        std::vector<ShaderPin> inputs;
        std::vector<ShaderPin> outputs;
        Vec2 editorPosition{};
        ShaderNodeValue value{};
        /** Used only by Property nodes. */
        std::optional<UUID> propertyId;
    };

    struct ShaderLink final {
        ShaderPinId fromPin{};
        ShaderPinId toPin{};
    };

    struct ShaderProperty final {
        UUID id{};
        std::string name;
        /** Stable shader identifier, e.g. baseColor. */
        std::string referenceName;
        ShaderValueType type{ShaderValueType::Float};
        ShaderNodeValue defaultValue{};
    };

    /** Serializable source asset. It intentionally owns no Vulkan state. */
    struct ShaderGraphAsset final {
        UUID id{};
        std::string name;
        std::vector<ShaderProperty> properties;
        std::vector<ShaderNode> nodes;
        std::vector<ShaderLink> links;
    };
} // namespace Engine
