#pragma once

// These marker types make GE_PROPERTY arguments intelligible to IDEs.  Their
// values are discarded by GE_PROPERTY; the script reflection generator reads
// the original source text and creates the runtime metadata.
namespace Engine::ScriptAttributes {
    struct Range {
        constexpr Range(double minimum, double maximum) noexcept {
            (void) minimum;
            (void) maximum;
        }
    };

    struct Min { constexpr explicit Min(double value) noexcept { (void) value; } };
    struct Max { constexpr explicit Max(double value) noexcept { (void) value; } };
    struct Step { constexpr explicit Step(double value) noexcept { (void) value; } };

    struct Tooltip {
        constexpr explicit Tooltip(const char* text) noexcept { (void) text; }
    };

    struct Header {
        constexpr explicit Header(const char* text) noexcept { (void) text; }
    };

    struct DisplayName { constexpr explicit DisplayName(const char* text) noexcept { (void) text; } };
    struct Space { constexpr explicit Space(float pixels = 8.0F) noexcept { (void) pixels; } };
    struct HideInInspector {};
    struct Multiline { constexpr explicit Multiline(int lines = 4) noexcept { (void) lines; } };
    struct Angle {};
    struct Percentage {};
    struct FormerlySerializedAs {
        constexpr explicit FormerlySerializedAs(const char* name) noexcept { (void) name; }
    };

    struct ReadOnly {};
}

// Keep script declarations concise and Unity-like while retaining declarations
// that CLion can resolve and use for parameter completion.
using Engine::ScriptAttributes::Header;
using Engine::ScriptAttributes::Min;
using Engine::ScriptAttributes::Max;
using Engine::ScriptAttributes::Step;
using Engine::ScriptAttributes::DisplayName;
using Engine::ScriptAttributes::Space;
using Engine::ScriptAttributes::HideInInspector;
using Engine::ScriptAttributes::Multiline;
using Engine::ScriptAttributes::Angle;
using Engine::ScriptAttributes::Percentage;
using Engine::ScriptAttributes::FormerlySerializedAs;
using Engine::ScriptAttributes::Range;
using Engine::ScriptAttributes::ReadOnly;
using Engine::ScriptAttributes::Tooltip;

// Consumed by GamEngine's script reflection generator.  It intentionally
// disappears before C++ compilation, leaving the field declaration unchanged.
#define GE_PROPERTY(...)
