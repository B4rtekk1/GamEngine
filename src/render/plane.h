#pragma once

#include "game_object.h"

class Plane final : public GameObject {
public:
    Plane() {
        mesh = {
            .vertices = {
                {{-0.5f, 0.0f, -0.5f}, {0.70f, 0.70f, 0.70f}, {0.0f, 0.0f}},
                {{ 0.5f, 0.0f, -0.5f}, {0.70f, 0.70f, 0.70f}, {1.0f, 0.0f}},
                {{ 0.5f, 0.0f,  0.5f}, {0.70f, 0.70f, 0.70f}, {1.0f, 1.0f}},
                {{-0.5f, 0.0f,  0.5f}, {0.70f, 0.70f, 0.70f}, {0.0f, 1.0f}},
            },
            .indices = {0, 1, 2, 2, 3, 0},
        };
    }
};
