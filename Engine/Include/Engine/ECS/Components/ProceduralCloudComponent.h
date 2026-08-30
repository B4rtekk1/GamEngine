#pragma once

#include "Engine/Math/Vec3.h"

#include <cstdint>

namespace Engine {
    /** Parameters used to deterministically build a soft, three-dimensional cloud mesh. */
    struct ProceduralCloudComponent final {
        std::uint32_t seed{1337U};
        std::uint32_t puffCount{32U};
        Vec3 dimensions{16.0F, 4.0F, 10.0F};
        float puffRadius{1.8F};
    };
} // namespace Engine
