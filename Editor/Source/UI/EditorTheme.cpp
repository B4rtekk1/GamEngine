#include "Editor/UI/EditorTheme.h"

namespace EditorUI {

const Palette& colors() {
    static const Palette palette{
        .appBackground = {0.043F, 0.055F, 0.071F, 1.0F},
        .titleBar = {0.031F, 0.043F, 0.059F, 1.0F},
        .panel = {0.063F, 0.078F, 0.098F, 1.0F},
        .surface = {0.082F, 0.102F, 0.125F, 1.0F},
        .surfaceRaised = {0.106F, 0.133F, 0.157F, 1.0F},
        .control = {0.090F, 0.114F, 0.141F, 1.0F},
        .controlHover = {0.133F, 0.169F, 0.192F, 1.0F},
        .border = {0.153F, 0.196F, 0.208F, 1.0F},
        .textPrimary = {0.906F, 0.925F, 0.953F, 1.0F},
        .textSecondary = {0.537F, 0.588F, 0.659F, 1.0F},
        .accent = {0.224F, 0.804F, 0.725F, 1.0F},
        .accentHover = {0.365F, 0.902F, 0.804F, 1.0F},
        .accentMuted = {0.110F, 0.302F, 0.290F, 1.0F},
        .selection = {0.224F, 0.804F, 0.725F, 0.24F},
        .selectionHover = {0.224F, 0.804F, 0.725F, 0.34F},
        .overlay = {0.035F, 0.047F, 0.063F, 0.82F},
        .overlayBorder = {1.0F, 1.0F, 1.0F, 0.08F},
        .success = {0.365F, 0.847F, 0.639F, 1.0F},
        .warning = {0.867F, 0.694F, 0.361F, 1.0F},
        .error = {0.941F, 0.424F, 0.424F, 1.0F},
        .iconMesh = {0.608F, 0.694F, 0.788F, 1.0F},
        .iconCamera = {0.475F, 0.651F, 0.957F, 1.0F},
        .iconLight = {0.957F, 0.788F, 0.388F, 1.0F},
        .iconTerrain = {0.537F, 0.784F, 0.431F, 1.0F},
        .iconWater = {0.333F, 0.722F, 0.937F, 1.0F},
    };
    return palette;
}

} // namespace EditorUI
