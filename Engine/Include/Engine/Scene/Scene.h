#pragma once

#include "Engine/Renderer/Geometry/Cube.h"
#include "Engine/Renderer/Geometry/Plane.h"
#include "Engine/Renderer/MeshRenderer.h"
#include "Engine/Core/Transform.h"
#include "Engine/ECS/Registry.h"
#include "Engine/ECS/Components/CameraComponent.h"
#include "Engine/UI/Canvas.h"
#include "Engine/UI/PanelElement.h"
#include "Engine/UI/TextElement.h"
#include "Engine/UI/Vulkan/UIFontAtlas.h"

#include <array>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

namespace Engine {

class Scene final {
public:
    static constexpr std::size_t CubesPerAxis = 30;
    static constexpr std::size_t CubeCount = CubesPerAxis * CubesPerAxis * CubesPerAxis;
    static constexpr float CubeSpacing = 1.25f;
    static constexpr float GridHalfExtent =
        ((CubesPerAxis - 1) * CubeSpacing + 1.0f) * 0.5f;

    Registry registry;
    UI::Canvas canvas{800, 600};
    UI::UIFontAtlas fontAtlas;
    Entity plane{NullEntity};
    Entity camera{NullEntity};
    std::array<Entity, CubeCount> cubes{};

private:
    std::shared_ptr<const Mesh> planeMesh;
    std::shared_ptr<const Mesh> cubeMesh;
    UI::TextElement* fpsTextElement = nullptr;

public:

    Scene() {
        planeMesh = std::make_shared<Mesh>(Plane::createMesh());
        cubeMesh = std::make_shared<Mesh>(Cube::createMesh());

        plane = registry.create();
        registry.add<Transform>(plane, Transform{
            .scale = {GridHalfExtent * 2.0f + 4.0f, 1.0f,
                      GridHalfExtent * 2.0f + 4.0f},
        });
        registry.add<MeshRenderer>(plane, MeshRenderer{
            .mesh = planeMesh,
        });

        camera = registry.create();
        constexpr float cameraTargetY = (CubesPerAxis - 1) * CubeSpacing * 0.5f + 0.5f;
        constexpr Vec3 cameraPosition{
            GridHalfExtent * 2.9f,
            cameraTargetY + GridHalfExtent * 2.6f,
            GridHalfExtent * 3.9f,
        };
        constexpr Vec3 cameraDirection = Vec3{0.0f, cameraTargetY, 0.0f} - cameraPosition;
        const float horizontalDistance = Vec2{cameraDirection.x(), cameraDirection.z()}.length();
        registry.add<Transform>(camera, Transform{
            .position = cameraPosition,
            .rotation = {
                Degrees{Radians{std::atan2(cameraDirection.y(), horizontalDistance)}}.value(),
                Degrees{Radians{std::atan2(cameraDirection.z(), cameraDirection.x())}}.value(),
                0.0f,
            },
        });
        registry.add<CameraComponent>(camera, CameraComponent{
            .fieldOfView = 45.0f,
            .nearClip = 0.1f,
            .farClip = 100'000.0f,
            .aspectRatio = 800.0f / 600.0f,
        });

        const std::array<std::filesystem::path, 5> fontCandidates{
            std::filesystem::path{"C:/Windows/Fonts/segoeui.ttf"},
            std::filesystem::path{"C:/Windows/Fonts/arial.ttf"},
            std::filesystem::path{"C:/Windows/Fonts/consola.ttf"},
            std::filesystem::path{"/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"},
            std::filesystem::path{"/usr/share/fonts/truetype/liberation2/LiberationSans-Regular.ttf"}
        };
        const auto font = std::find_if(fontCandidates.begin(), fontCandidates.end(),
                                       [](const auto& candidate) {
                                           return std::filesystem::exists(candidate);
                                       });
        if (font == fontCandidates.end()) {
            throw std::runtime_error("Could not find a system TrueType font for the FPS HUD");
        }
        if (const std::string error = fontAtlas.build(font->string(), 24, 32, 126);
            !error.empty()) {
            throw std::runtime_error("Could not build FPS font atlas: " + error);
        }
        const auto nonZeroPixels = std::count_if(fontAtlas.pixels().begin(), fontAtlas.pixels().end(),
                                                 [](const std::uint8_t pixel) { return pixel != 0; });
        std::cout << "FPS HUD font: " << font->string()
                  << " (non-zero atlas pixels: " << nonZeroPixels
                  << ", glyph F: " << (fontAtlas.glyph('F') != nullptr ? "yes" : "no") << ")\n";

        auto panel = std::make_unique<UI::PanelElement>(
            Math::Color{0.025f, 0.035f, 0.055f, 0.82f});
        panel->rectTransform.anchorMin = {0.0f, 0.0f};
        panel->rectTransform.anchorMax = {0.0f, 0.0f};
        panel->rectTransform.offsetMin = {20.0f, 20.0f};
        panel->rectTransform.offsetMax = {300.0f, 110.0f};

        auto accent = std::make_unique<UI::PanelElement>(
            Math::Color{0.10f, 0.75f, 0.90f, 1.0f});
        accent->rectTransform.anchorMin = {0.0f, 0.0f};
        accent->rectTransform.anchorMax = {0.0f, 1.0f};
        accent->rectTransform.offsetMin = {0.0f, 0.0f};
        accent->rectTransform.offsetMax = {4.0f, 0.0f};
        panel->addChild(std::move(accent));

        TextComponent fpsText;
        fpsText.text = "FPS: --";
        fpsText.fontSize = 24.0f;
        fpsText.color = Math::Color{0.90f, 0.96f, 1.0f, 1.0f};
        auto textElement = std::make_unique<UI::TextElement>(fpsText, fontAtlas);
        fpsTextElement = textElement.get();
        textElement->rectTransform.anchorMin = {0.0f, 0.0f};
        textElement->rectTransform.anchorMax = {1.0f, 1.0f};
        textElement->rectTransform.offsetMin = {14.0f, 12.0f};
        textElement->rectTransform.offsetMax = {-8.0f, -8.0f};
        panel->addChild(std::move(textElement));
        static_cast<void>(canvas.addElement(std::move(panel)));

        constexpr float halfGridWidth = (CubesPerAxis - 1) * CubeSpacing * 0.5f;

        for (std::size_t layer = 0; layer < CubesPerAxis; ++layer) {
            for (std::size_t row = 0; row < CubesPerAxis; ++row) {
                for (std::size_t column = 0; column < CubesPerAxis; ++column) {
                    const std::size_t index =
                        (layer * CubesPerAxis + row) * CubesPerAxis + column;
                    const Entity cube = registry.create();
                    cubes[index] = cube;

                    registry.add<Transform>(cube, Transform{
                        .position = {
                            static_cast<float>(column) * CubeSpacing - halfGridWidth,
                            static_cast<float>(layer) * CubeSpacing + 0.5f,
                            static_cast<float>(row) * CubeSpacing - halfGridWidth,
                        },
                    });
                    registry.add<MeshRenderer>(cube, MeshRenderer{
                        .mesh = cubeMesh,
                        .material = {
                            .baseColor = {0.72f, 0.72f, 0.72f},
                            .metallic = 0.05f,
                            .roughness = 0.62f,
                        },
                        .castShadow = false,
                    });
                }
            }
        }
    }

    [[nodiscard]] UI::Canvas& uiCanvas() noexcept { return canvas; }
    [[nodiscard]] const UI::UIFontAtlas& uiFontAtlas() const noexcept { return fontAtlas; }

    void setFps(const double fps) {
        if (fpsTextElement == nullptr) return;
        char text[64];
        std::snprintf(text, sizeof(text), "FPS: %.1f", fps);
        fpsTextElement->text.text = text;
    }
};

} // namespace Engine
