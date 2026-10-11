#pragma once

#include <SDL3/SDL.h>

#include <filesystem>

namespace Platform {
inline std::filesystem::path defaultUiFont() {
    if (const char* base = SDL_GetBasePath()) {
        const auto bundled = std::filesystem::path{base} / "Fonts/DroidSans.ttf";
        if (std::filesystem::is_regular_file(bundled)) return bundled;
    }
    for (const auto* path : {
             "C:/Windows/Fonts/segoeui.ttf", "C:/Windows/Fonts/arial.ttf",
             "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
             "/usr/share/fonts/TTF/DejaVuSans.ttf",
             "/usr/share/fonts/truetype/liberation2/LiberationSans-Regular.ttf",
             "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf"}) {
        if (std::filesystem::is_regular_file(path)) return path;
    }
    return {};
}
} // namespace Platform
