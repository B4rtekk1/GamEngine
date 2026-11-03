#pragma once

#include "Engine/Scene/ScenePresets.h"
#include "imgui.h"

#include <cstdint>
#include <optional>

namespace EditorUI {

/**
 * Vector icons drawn with ImDrawList.
 *
 * The bundled UI font only covers Latin text, so geometric Unicode glyphs
 * rendered as missing-glyph boxes. Drawing icons keeps them crisp at any DPI
 * and independent of whichever system font was found.
 */
enum class Icon : std::uint8_t {
    GameObject,
    Mesh,
    Camera,
    Light,
    Terrain,
    Water,
    Cloud,
    Particles,
    Play,
    Pause,
    Stop,
    Restart,
    Eye,
    EyeOff,
    Lock,
    Unlock,
    Search,
    Plus,
    Move,
    Rotate,
    Scale,
    BoxSelect,
    Lasso,
    Focus,
    Frame,
    Bookmark,
    Info,
    Warning,
    Error,
    Gear,
    Layers,
};

/** Draws @p icon centred on @p center inside a @p size x @p size box. */
void drawIcon(ImDrawList* drawList, Icon icon, ImVec2 center, float size, ImU32 color);

struct IconButtonOptions final {
    bool active = false;
    /** Zero uses a square of the current frame height. */
    ImVec2 size{};
    /** Overrides the icon colour, e.g. green Play or red Stop. */
    std::optional<ImVec4> tint;
    /** Draws an opaque pill suitable for placing over the rendered viewport. */
    bool overlay = false;
    /** Overlay tools must not steal keyboard navigation from the scene. */
    bool noNavFocus = false;
};

/** Flat toolbar button showing only an icon; returns true when clicked. */
bool iconButton(const char* id, Icon icon, const char* tooltip = nullptr, const IconButtonOptions& options = {});

/** Draws an inline icon aligned with the following text item. */
void iconLabel(Icon icon, ImVec4 color, float size = 0.0F);

/** Picks the icon that best describes an entity's primary component. */
[[nodiscard]] Icon entityIcon(const Engine::ScenePreset& scene, Engine::Entity entity);
[[nodiscard]] ImVec4 entityIconColor(Icon icon);

} // namespace EditorUI
