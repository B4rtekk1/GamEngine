#include "Editor/UI/EditorTheme.h"

namespace EditorUI {

const Palette& colors() {
    static const Palette palette{
        .background = {0.060F, 0.070F, 0.095F, 1.0F},
        .surface = {0.044F, 0.052F, 0.074F, 1.0F},
        .surfaceRaised = {0.078F, 0.092F, 0.128F, 1.0F},
        .text = {0.910F, 0.940F, 0.990F, 1.0F},
        .textMuted = {0.470F, 0.555F, 0.680F, 1.0F},
        .accent = {0.105F, 0.640F, 0.835F, 1.0F},
        .accentHovered = {0.110F, 0.530F, 0.720F, 1.0F},
        .success = {0.330F, 0.900F, 0.840F, 1.0F},
        .warning = {0.960F, 0.720F, 0.280F, 1.0F},
        .error = {0.940F, 0.360F, 0.360F, 1.0F},
        .border = {0.170F, 0.215F, 0.290F, 0.780F},
    };
    return palette;
}

} // namespace EditorUI
