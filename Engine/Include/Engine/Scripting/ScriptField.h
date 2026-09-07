#pragma once

#include "Engine/Math/Color.h"
#include "Engine/Math/Vec3.h"

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace Engine {
    class Script;

    enum class ScriptFieldType : std::uint8_t { Bool, Int, Float, Double, Vec3, Color, String };
    using ScriptFieldValue = std::variant<bool, int, float, double, Vec3, Color, std::string>;

    struct ScriptRangeAttribute {
        double min{};
        double max{};
    };

    struct ScriptTooltipAttribute {
        std::string text;
    };

    struct ScriptHeaderAttribute {
        std::string text;
    };

    struct ScriptReadOnlyAttribute {};
    struct ScriptMinAttribute { double value{}; };
    struct ScriptMaxAttribute { double value{}; };
    struct ScriptStepAttribute { double value{}; };
    struct ScriptDisplayNameAttribute { std::string text; };
    struct ScriptHideInInspectorAttribute {};
    struct ScriptMultilineAttribute { int lines = 4; };
    struct ScriptAngleAttribute {};
    struct ScriptPercentageAttribute {};
    struct ScriptSpaceAttribute { float pixels = 8.0F; };
    struct ScriptFormerlySerializedAsAttribute { std::string name; };

    using ScriptFieldAttribute = std::variant<
        ScriptRangeAttribute,
        ScriptTooltipAttribute,
        ScriptHeaderAttribute,
        ScriptReadOnlyAttribute,
        ScriptMinAttribute,
        ScriptMaxAttribute,
        ScriptStepAttribute,
        ScriptDisplayNameAttribute,
        ScriptHideInInspectorAttribute,
        ScriptMultilineAttribute,
        ScriptAngleAttribute,
        ScriptPercentageAttribute,
        ScriptSpaceAttribute,
        ScriptFormerlySerializedAsAttribute>;

    struct ScriptFieldDescriptor {
        std::string name;
        ScriptFieldType type{};
        ScriptFieldValue defaultValue{};
        std::vector<ScriptFieldAttribute> attributes;
        using Read = void (*)(const Script*, ScriptFieldValue&);
        using Write = bool (*)(Script*, const ScriptFieldValue&);
        Read read{};
        Write write{};
    };

    template<typename T>
    [[nodiscard]] const T* findScriptAttribute(const ScriptFieldDescriptor& field) {
        for (const auto& attribute : field.attributes) {
            if (const auto* result = std::get_if<T>(&attribute)) return result;
        }
        return nullptr;
    }

    template<typename T>
    constexpr ScriptFieldType scriptFieldType() {
        static_assert(sizeof(T) == 0, "Unsupported reflected script field type");
    }
#define ENGINE_SCRIPT_FIELD_TYPE(type, value) \
    template<> constexpr ScriptFieldType scriptFieldType<type>() { return ScriptFieldType::value; }
    ENGINE_SCRIPT_FIELD_TYPE(bool, Bool)
    ENGINE_SCRIPT_FIELD_TYPE(int, Int)
    ENGINE_SCRIPT_FIELD_TYPE(float, Float)
    ENGINE_SCRIPT_FIELD_TYPE(double, Double)
    ENGINE_SCRIPT_FIELD_TYPE(Vec3, Vec3)
    ENGINE_SCRIPT_FIELD_TYPE(Color, Color)
    ENGINE_SCRIPT_FIELD_TYPE(std::string, String)
#undef ENGINE_SCRIPT_FIELD_TYPE
}
