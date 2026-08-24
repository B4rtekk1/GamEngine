#include "Editor/Panels/BlueprintOverlay.h"

#include "Engine/Core/Camera.h"
#include "Engine/Math/Vec3.h"
#include "Engine/Renderer/Renderer.h"

#include <cmath>

namespace Editor {
namespace {

float dotProduct(const Engine::Vec3& lhs, const Engine::Vec3& rhs) {
    return lhs.x() * rhs.x() + lhs.y() * rhs.y() + lhs.z() * rhs.z();
}

ImVec2 projectPoint(const Engine::Camera& camera, const Engine::Vec3& point,
                    const ImVec2 min, const ImVec2 max) {
    const Engine::Vec3 relative = point - camera.position();
    const float depth = dotProduct(relative, camera.forward());
    if (depth <= 0.01f) return {-10000.0f, -10000.0f};
    constexpr float halfFovTangent = 0.57735026919f; // tan(60 degrees / 2)
    const float aspect = (max.x - min.x) / (max.y - min.y);
    const float ndcX = dotProduct(relative, camera.right()) / (depth * halfFovTangent * aspect);
    const float ndcY = dotProduct(relative, camera.up()) / (depth * halfFovTangent);
    return {(min.x + max.x) * 0.5f + ndcX * (max.x - min.x) * 0.5f,
            (min.y + max.y) * 0.5f - ndcY * (max.y - min.y) * 0.5f};
}

} // namespace

void drawBlueprintOverlay(const Engine::Renderer& renderer, const ImVec2 min, const ImVec2 max) {
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->PushClipRect(min, max, true);

    Engine::Camera camera{Engine::Degrees{60.0f}, (max.x - min.x) / (max.y - min.y),
                          0.1f, 1000.0f};
    camera.setPosition(renderer.editorCameraPosition());
    camera.setRotation(Engine::Degrees{renderer.editorCameraYaw()},
                       Engine::Degrees{renderer.editorCameraPitch()});

    const auto drawWorldLine = [&](Engine::Vec3 start, Engine::Vec3 end,
                                   const ImU32 color, const float thickness) {
        constexpr float nearDepth = 0.11f;
        const float startDepth = dotProduct(start - camera.position(), camera.forward());
        const float endDepth = dotProduct(end - camera.position(), camera.forward());
        if (startDepth <= nearDepth && endDepth <= nearDepth) return;
        if (startDepth <= nearDepth || endDepth <= nearDepth) {
            const float interpolation = (nearDepth - startDepth) / (endDepth - startDepth);
            const Engine::Vec3 clipped = start + (end - start) * interpolation;
            if (startDepth <= nearDepth) start = clipped;
            else end = clipped;
        }
        drawList->AddLine(projectPoint(camera, start, min, max),
                          projectPoint(camera, end, min, max), color, thickness);
    };

    constexpr int halfExtent = 40;
    constexpr int majorEvery = 5;
    const ImU32 minorColor = IM_COL32(99, 202, 235, 36);
    const ImU32 majorColor = IM_COL32(114, 220, 248, 72);
    for (int coordinate = -halfExtent; coordinate <= halfExtent; ++coordinate) {
        const bool major = std::abs(coordinate) % majorEvery == 0;
        const ImU32 color = major ? majorColor : minorColor;
        const float thickness = major ? 1.25f : 1.0f;
        drawWorldLine({static_cast<float>(coordinate), 0.0f, -halfExtent},
                      {static_cast<float>(coordinate), 0.0f, halfExtent}, color, thickness);
        drawWorldLine({-halfExtent, 0.0f, static_cast<float>(coordinate)},
                      {halfExtent, 0.0f, static_cast<float>(coordinate)}, color, thickness);
    }

    drawWorldLine({-halfExtent, 0.0f, 0.0f}, {halfExtent, 0.0f, 0.0f},
                  IM_COL32(244, 112, 112, 150), 1.75f);
    drawWorldLine({0.0f, 0.0f, -halfExtent}, {0.0f, 0.0f, halfExtent},
                  IM_COL32(110, 174, 255, 160), 1.75f);
    drawList->PopClipRect();
}

} // namespace Editor
