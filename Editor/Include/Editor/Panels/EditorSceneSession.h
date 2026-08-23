#pragma once

#include "Engine/Scene/ScenePresets.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace Engine { class Renderer; }

class EditorSceneSession final {
public:
    [[nodiscard]] static std::filesystem::path scenePath();
    [[nodiscard]] static std::uint32_t msaaSampleCount(const Engine::Renderer& renderer);

    static bool setPlayMode(bool play, Engine::ScenePreset& scene, std::string& snapshot,
                            std::string& error, std::uint32_t msaaSamples);
    static bool createCppScript(std::string_view name, std::string& error);
};
