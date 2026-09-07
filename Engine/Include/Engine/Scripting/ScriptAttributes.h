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

    struct Tooltip {
        constexpr explicit Tooltip(const char* text) noexcept { (void) text; }
    };

    struct Header {
        constexpr explicit Header(const char* text) noexcept { (void) text; }
    };

    struct ReadOnly {};
}

// Keep script declarations concise and Unity-like while retaining declarations
// that CLion can resolve and use for parameter completion.
using Engine::ScriptAttributes::Header;
using Engine::ScriptAttributes::Range;
using Engine::ScriptAttributes::ReadOnly;
using Engine::ScriptAttributes::Tooltip;

// Consumed by GamEngine's script reflection generator.  It intentionally
// disappears before C++ compilation, leaving the field declaration unchanged.
#define GE_PROPERTY(...)
