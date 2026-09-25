#pragma once

#include "Engine/Renderer/Vulkan/hdr_buffer.h"
#include "Engine/Renderer/Vulkan/buffer.h"

#include <array>

namespace Engine {
    struct DDGIResources final {
        struct Frame final {
            HdrBuffer rayData;
            HdrBuffer irradiance;
            HdrBuffer distance;
            HdrBuffer fixedRayData;
            HdrBuffer probeData;
            Buffer probeStates;
            Buffer updateList;
        };
        struct Cascade final { Frame history; };
        std::array<Cascade, 3> cascades{};
    };
}
