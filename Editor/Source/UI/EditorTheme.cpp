#include "Editor/UI/EditorTheme.h"

namespace EditorUI {

const Palette& colors() {
    static const Palette palette{
        .appBackground = {0.051F, 0.063F, 0.086F, 1.0F},
        .titleBar = {0.039F, 0.051F, 0.071F, 1.0F},
        .panel = {0.071F, 0.090F, 0.133F, 1.0F},
        .surface = {0.090F, 0.114F, 0.153F, 1.0F},
        .surfaceRaised = {0.110F, 0.141F, 0.188F, 1.0F},
        .control = {0.094F, 0.118F, 0.153F, 1.0F},
        .controlHover = {0.137F, 0.176F, 0.227F, 1.0F},
        .border = {0.161F, 0.196F, 0.251F, 1.0F},
        .textPrimary = {0.906F, 0.925F, 0.953F, 1.0F},
        .textSecondary = {0.537F, 0.588F, 0.659F, 1.0F},
        .accent = {0.298F, 0.616F, 1.000F, 1.0F},
        .accentHover = {0.420F, 0.698F, 1.000F, 1.0F},
        .success = {0.310F, 0.816F, 0.639F, 1.0F},
        .warning = {0.867F, 0.694F, 0.361F, 1.0F},
        .error = {0.941F, 0.424F, 0.424F, 1.0F},
    };
    return palette;
}

} // namespace EditorUI
