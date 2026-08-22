#pragma once

#include "Engine/Math/Color.h"

namespace Engine {

/** Editable RGBA color exposed as a color picker in the editor. */
struct ColorPickerComponent final {
    Color color{Color::white()};
};

} // namespace Engine
