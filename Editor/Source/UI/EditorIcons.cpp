#include "Editor/UI/EditorIcons.h"

#include "Editor/UI/EditorTheme.h"
#include "Engine/ECS/Components/CameraComponent.h"
#include "Engine/ECS/Components/ParticleEmitterComponent.h"
#include "Engine/ECS/Components/ProceduralCloudComponent.h"
#include "Engine/ECS/Components/SmokeEmitterComponent.h"
#include "Engine/ECS/Components/TerrainComponent.h"
#include "Engine/ECS/Components/WaterBodyComponent.h"
#include "Engine/Renderer/MeshRenderer.h"
#include "Engine/Scene/Components/LightComponent.h"
#include "imgui_internal.h"

#include <algorithm>
#include <array>
#include <cmath>

// NOLINTBEGIN(readability-magic-numbers)

namespace EditorUI {
namespace {

constexpr float kPi = 3.14159265358979F;

/** Maps unit-box coordinates (-0.5..0.5) to screen space. */
struct IconFrame final {
    ImVec2 center;
    float size;

    [[nodiscard]] ImVec2 operator()(const float x, const float y) const {
        return {center.x + x * size, center.y + y * size};
    }
    [[nodiscard]] float stroke() const {
        return std::max(1.0F, size * 0.095F);
    }
};

void drawIsoCube(ImDrawList* drawList, const IconFrame& p, const ImU32 color, const bool filledTop) {
    const ImVec2 top = p(0.0F, -0.42F);
    const ImVec2 upperRight = p(0.38F, -0.21F);
    const ImVec2 lowerRight = p(0.38F, 0.21F);
    const ImVec2 bottom = p(0.0F, 0.42F);
    const ImVec2 lowerLeft = p(-0.38F, 0.21F);
    const ImVec2 upperLeft = p(-0.38F, -0.21F);
    const ImVec2 middle = p(0.0F, 0.0F);
    if (filledTop) {
        const ImVec4 fill = ImGui::ColorConvertU32ToFloat4(color);
        drawList->AddQuadFilled(top, upperRight, middle, upperLeft,
                                ImGui::GetColorU32(ImVec4{fill.x, fill.y, fill.z, fill.w * 0.55F}));
    }
    const std::array outline{top, upperRight, lowerRight, bottom, lowerLeft, upperLeft};
    drawList->AddPolyline(outline.data(), static_cast<int>(outline.size()), color, ImDrawFlags_Closed, p.stroke());
    drawList->AddLine(upperLeft, middle, color, p.stroke());
    drawList->AddLine(upperRight, middle, color, p.stroke());
    drawList->AddLine(middle, bottom, color, p.stroke());
}

void drawArrowHead(ImDrawList* drawList, const ImVec2 tip, const float dirX, const float dirY, const float length,
                   const ImU32 color) {
    const ImVec2 base{tip.x - dirX * length, tip.y - dirY * length};
    const float normalX = -dirY * length * 0.75F;
    const float normalY = dirX * length * 0.75F;
    drawList->AddTriangleFilled(tip, {base.x + normalX, base.y + normalY}, {base.x - normalX, base.y - normalY}, color);
}

void drawEyeShape(ImDrawList* drawList, const IconFrame& p, const ImU32 color) {
    constexpr int segments = 12;
    std::array<ImVec2, segments * 2> points{};
    for (int index = 0; index < segments; ++index) {
        const float t = -1.0F + 2.0F * static_cast<float>(index) / static_cast<float>(segments);
        points[static_cast<std::size_t>(index)] = p(t * 0.46F, -0.26F * (1.0F - t * t));
        points[static_cast<std::size_t>(index + segments)] = p(-t * 0.46F, 0.26F * (1.0F - t * t));
    }
    drawList->AddPolyline(points.data(), static_cast<int>(points.size()), color, ImDrawFlags_Closed, p.stroke());
    drawList->AddCircleFilled(p(0.0F, 0.0F), p.size * 0.13F, color);
}

void drawLock(ImDrawList* drawList, const IconFrame& p, const ImU32 color, const bool open) {
    drawList->AddRectFilled(p(-0.32F, -0.04F), p(0.32F, 0.42F), color, p.size * 0.08F);
    const float shiftX = open ? 0.22F : 0.0F;
    drawList->PathArcTo(p(shiftX, -0.16F), p.size * 0.2F, kPi, 2.0F * kPi, 10);
    drawList->PathStroke(color, ImDrawFlags_None, p.stroke() * 1.15F);
    drawList->AddLine(p(shiftX - 0.2F, -0.16F), p(shiftX - 0.2F, -0.04F), color, p.stroke() * 1.15F);
    if (!open)
        drawList->AddLine(p(0.2F, -0.16F), p(0.2F, -0.04F), color, p.stroke() * 1.15F);
}

void drawCornerBrackets(ImDrawList* drawList, const IconFrame& p, const ImU32 color, const float extent,
                        const float arm) {
    for (const float sx : {-1.0F, 1.0F}) {
        for (const float sy : {-1.0F, 1.0F}) {
            const ImVec2 corner = p(sx * extent, sy * extent);
            drawList->AddLine(corner, p(sx * (extent - arm), sy * extent), color, p.stroke());
            drawList->AddLine(corner, p(sx * extent, sy * (extent - arm)), color, p.stroke());
        }
    }
}

void drawBadge(ImDrawList* drawList, const IconFrame& p, const ImU32 color, const Icon icon) {
    const ImU32 cutout = ImGui::GetColorU32(colors().appBackground);
    if (icon == Icon::Warning) {
        drawList->AddTriangleFilled(p(0.0F, -0.44F), p(0.48F, 0.40F), p(-0.48F, 0.40F), color);
        drawList->AddLine(p(0.0F, -0.14F), p(0.0F, 0.12F), cutout, p.stroke() * 1.3F);
        drawList->AddCircleFilled(p(0.0F, 0.26F), p.size * 0.06F, cutout);
        return;
    }
    drawList->AddCircleFilled(p(0.0F, 0.0F), p.size * 0.46F, color, 20);
    if (icon == Icon::Info) {
        drawList->AddCircleFilled(p(0.0F, -0.2F), p.size * 0.06F, cutout);
        drawList->AddLine(p(0.0F, -0.06F), p(0.0F, 0.24F), cutout, p.stroke() * 1.3F);
    } else {
        drawList->AddLine(p(-0.16F, -0.16F), p(0.16F, 0.16F), cutout, p.stroke() * 1.3F);
        drawList->AddLine(p(0.16F, -0.16F), p(-0.16F, 0.16F), cutout, p.stroke() * 1.3F);
    }
}

} // namespace

void drawIcon(ImDrawList* drawList, const Icon icon, const ImVec2 center, const float size, const ImU32 color) {
    const IconFrame p{center, size};
    const float stroke = p.stroke();
    switch (icon) {
    case Icon::GameObject:
        drawIsoCube(drawList, p, color, false);
        break;
    case Icon::Mesh:
        drawIsoCube(drawList, p, color, true);
        break;
    case Icon::Camera:
        drawList->AddRectFilled(p(-0.46F, -0.24F), p(0.14F, 0.28F), color, size * 0.08F);
        drawList->AddTriangleFilled(p(0.14F, 0.02F), p(0.46F, -0.2F), p(0.46F, 0.24F), color);
        drawList->AddCircleFilled(p(-0.3F, -0.36F), size * 0.1F, color);
        drawList->AddCircleFilled(p(-0.04F, -0.36F), size * 0.1F, color);
        break;
    case Icon::Light:
        drawList->AddCircleFilled(p(0.0F, 0.0F), size * 0.18F, color, 16);
        for (int ray = 0; ray < 8; ++ray) {
            const float angle = static_cast<float>(ray) * kPi * 0.25F;
            const float cosine = std::cos(angle);
            const float sine = std::sin(angle);
            drawList->AddLine(p(cosine * 0.29F, sine * 0.29F), p(cosine * 0.45F, sine * 0.45F), color, stroke);
        }
        break;
    case Icon::Terrain: {
        const std::array mountain{p(-0.48F, 0.32F), p(-0.18F, -0.16F), p(-0.02F, 0.06F), p(0.16F, -0.32F),
                                  p(0.48F, 0.32F)};
        drawList->AddConvexPolyFilled(mountain.data() + 2, 3, color);
        drawList->AddPolyline(mountain.data(), static_cast<int>(mountain.size()), color, ImDrawFlags_Closed, stroke);
        drawList->AddTriangleFilled(mountain[0], mountain[1], p(0.06F, 0.32F), color);
        break;
    }
    case Icon::Water:
        for (const float y : {-0.22F, 0.0F, 0.22F}) {
            constexpr int segments = 12;
            std::array<ImVec2, segments + 1> wave{};
            for (int index = 0; index <= segments; ++index) {
                const float t = static_cast<float>(index) / static_cast<float>(segments);
                wave[static_cast<std::size_t>(index)] = p(-0.46F + t * 0.92F, y + std::sin(t * 2.0F * kPi) * 0.07F);
            }
            drawList->AddPolyline(wave.data(), static_cast<int>(wave.size()), color, ImDrawFlags_None, stroke);
        }
        break;
    case Icon::Cloud:
        drawList->AddCircleFilled(p(-0.2F, 0.08F), size * 0.2F, color, 16);
        drawList->AddCircleFilled(p(0.06F, -0.06F), size * 0.26F, color, 16);
        drawList->AddCircleFilled(p(0.26F, 0.12F), size * 0.18F, color, 16);
        drawList->AddRectFilled(p(-0.2F, 0.1F), p(0.26F, 0.3F), color);
        break;
    case Icon::Particles:
        for (const auto& [x, y, radius] : std::array<std::array<float, 3>, 6>{{{-0.3F, 0.26F, 0.1F},
                                                                               {0.02F, 0.3F, 0.08F},
                                                                               {0.3F, 0.18F, 0.1F},
                                                                               {-0.14F, -0.04F, 0.08F},
                                                                               {0.18F, -0.14F, 0.07F},
                                                                               {-0.02F, -0.36F, 0.06F}}}) {
            drawList->AddCircleFilled(p(x, y), size * radius, color, 10);
        }
        break;
    case Icon::Play:
        drawList->AddTriangleFilled(p(-0.26F, -0.36F), p(0.38F, 0.0F), p(-0.26F, 0.36F), color);
        break;
    case Icon::Pause:
        drawList->AddRectFilled(p(-0.3F, -0.34F), p(-0.08F, 0.34F), color, size * 0.04F);
        drawList->AddRectFilled(p(0.08F, -0.34F), p(0.3F, 0.34F), color, size * 0.04F);
        break;
    case Icon::Stop:
        drawList->AddRectFilled(p(-0.3F, -0.3F), p(0.3F, 0.3F), color, size * 0.06F);
        break;
    case Icon::Restart: {
        constexpr float arcStart = -0.35F * kPi;
        constexpr float arcEnd = 1.35F * kPi;
        drawList->PathArcTo(p(0.0F, 0.0F), size * 0.32F, arcStart, arcEnd, 20);
        drawList->PathStroke(color, ImDrawFlags_None, stroke * 1.2F);
        const ImVec2 tip = p(std::cos(arcStart) * 0.32F, std::sin(arcStart) * 0.32F);
        drawArrowHead(drawList, {tip.x + size * 0.06F, tip.y + size * 0.02F}, 0.75F, 0.66F, size * 0.2F, color);
        break;
    }
    case Icon::Eye:
        drawEyeShape(drawList, p, color);
        break;
    case Icon::EyeOff:
        drawEyeShape(drawList, p, color);
        drawList->AddLine(p(-0.42F, 0.4F), p(0.42F, -0.4F), color, stroke * 1.2F);
        break;
    case Icon::Lock:
        drawLock(drawList, p, color, false);
        break;
    case Icon::Unlock:
        drawLock(drawList, p, color, true);
        break;
    case Icon::Search:
        drawList->AddCircle(p(-0.08F, -0.08F), size * 0.28F, color, 16, stroke * 1.2F);
        drawList->AddLine(p(0.13F, 0.13F), p(0.42F, 0.42F), color, stroke * 1.5F);
        break;
    case Icon::Plus:
        drawList->AddLine(p(-0.34F, 0.0F), p(0.34F, 0.0F), color, stroke * 1.3F);
        drawList->AddLine(p(0.0F, -0.34F), p(0.0F, 0.34F), color, stroke * 1.3F);
        break;
    case Icon::Move: {
        constexpr float arm = 0.3F;
        constexpr float head = 0.14F;
        drawList->AddLine(p(-arm, 0.0F), p(arm, 0.0F), color, stroke);
        drawList->AddLine(p(0.0F, -arm), p(0.0F, arm), color, stroke);
        drawArrowHead(drawList, p(arm + head, 0.0F), 1.0F, 0.0F, size * head, color);
        drawArrowHead(drawList, p(-arm - head, 0.0F), -1.0F, 0.0F, size * head, color);
        drawArrowHead(drawList, p(0.0F, arm + head), 0.0F, 1.0F, size * head, color);
        drawArrowHead(drawList, p(0.0F, -arm - head), 0.0F, -1.0F, size * head, color);
        drawList->AddRectFilled(p(-0.08F, -0.08F), p(0.08F, 0.08F), color, size * 0.03F);
        break;
    }
    case Icon::Rotate: {
        constexpr float arcStart = 0.18F * kPi;
        constexpr float arcEnd = 1.77F * kPi;
        drawList->PathArcTo(p(0.0F, 0.0F), size * 0.32F, arcStart, arcEnd, 20);
        drawList->PathStroke(color, ImDrawFlags_None, stroke * 1.2F);
        const ImVec2 tip = p(std::cos(arcEnd) * 0.32F, std::sin(arcEnd) * 0.32F);
        drawArrowHead(drawList, tip, -std::sin(arcEnd), std::cos(arcEnd), size * 0.2F, color);
        break;
    }
    case Icon::Scale:
        drawList->AddRect(p(-0.4F, -0.4F), p(0.4F, 0.4F), color, size * 0.05F, 0, stroke);
        drawList->AddRectFilled(p(-0.4F, 0.02F), p(-0.02F, 0.4F), color, size * 0.04F);
        drawList->AddLine(p(0.02F, -0.02F), p(0.28F, -0.28F), color, stroke);
        drawArrowHead(drawList, p(0.3F, -0.3F), 0.707F, -0.707F, size * 0.16F, color);
        break;
    case Icon::BoxSelect:
        for (int dash = 0; dash < 4; ++dash) {
            const float from = -0.4F + static_cast<float>(dash) * 0.22F;
            const float to = from + 0.13F;
            drawList->AddLine(p(from, -0.36F), p(to, -0.36F), color, stroke);
            drawList->AddLine(p(from, 0.36F), p(to, 0.36F), color, stroke);
            drawList->AddLine(p(-0.4F, from * 0.9F), p(-0.4F, to * 0.9F), color, stroke);
            drawList->AddLine(p(0.4F, from * 0.9F), p(0.4F, to * 0.9F), color, stroke);
        }
        break;
    case Icon::Lasso: {
        constexpr int segments = 18;
        std::array<ImVec2, segments + 1> loop{};
        for (int index = 0; index <= segments; ++index) {
            const float angle = static_cast<float>(index) / static_cast<float>(segments) * 1.8F * kPi + 0.6F * kPi;
            const float radius = 0.34F + 0.06F * std::sin(angle * 3.0F);
            loop[static_cast<std::size_t>(index)] =
                p(std::cos(angle) * radius, std::sin(angle) * radius * 0.72F - 0.06F);
        }
        drawList->AddPolyline(loop.data(), static_cast<int>(loop.size()), color, ImDrawFlags_None, stroke);
        drawList->AddBezierQuadratic(loop.back(), p(-0.1F, 0.44F), p(0.12F, 0.46F), color, stroke);
        break;
    }
    case Icon::Focus:
        drawCornerBrackets(drawList, p, color, 0.4F, 0.16F);
        drawList->AddCircle(p(0.0F, 0.0F), size * 0.14F, color, 12, stroke);
        break;
    case Icon::Frame:
        drawCornerBrackets(drawList, p, color, 0.42F, 0.18F);
        drawList->AddRect(p(-0.18F, -0.18F), p(0.18F, 0.18F), color, size * 0.03F, 0, stroke);
        break;
    case Icon::Bookmark: {
        const std::array ribbon{p(-0.26F, -0.42F), p(0.26F, -0.42F), p(0.26F, 0.42F), p(0.0F, 0.2F), p(-0.26F, 0.42F)};
        drawList->AddPolyline(ribbon.data(), static_cast<int>(ribbon.size()), color, ImDrawFlags_Closed, stroke);
        break;
    }
    case Icon::Info:
    case Icon::Warning:
    case Icon::Error:
        drawBadge(drawList, p, color, icon);
        break;
    case Icon::Gear:
        drawList->AddCircle(p(0.0F, 0.0F), size * 0.24F, color, 16, stroke * 1.6F);
        for (int tooth = 0; tooth < 8; ++tooth) {
            const float angle = static_cast<float>(tooth) * kPi * 0.25F;
            drawList->AddLine(p(std::cos(angle) * 0.3F, std::sin(angle) * 0.3F),
                              p(std::cos(angle) * 0.44F, std::sin(angle) * 0.44F), color, stroke * 1.8F);
        }
        break;
    case Icon::Layers:
        for (const float y : {0.14F, -0.06F}) {
            const std::array layer{p(0.0F, y - 0.2F), p(0.44F, y), p(0.0F, y + 0.2F), p(-0.44F, y)};
            drawList->AddPolyline(layer.data(), static_cast<int>(layer.size()), color, ImDrawFlags_Closed, stroke);
        }
        break;
    case Icon::Back:
        drawList->AddLine(p(0.36F, 0.0F), p(-0.3F, 0.0F), color, stroke * 1.3F);
        drawList->AddLine(p(-0.36F, 0.0F), p(-0.06F, -0.3F), color, stroke * 1.3F);
        drawList->AddLine(p(-0.36F, 0.0F), p(-0.06F, 0.3F), color, stroke * 1.3F);
        break;
    case Icon::Refresh: {
        constexpr float arcStart = -0.1F * kPi;
        constexpr float arcEnd = 1.45F * kPi;
        drawList->PathArcTo(p(0.0F, 0.0F), size * 0.32F, arcStart, arcEnd, 20);
        drawList->PathStroke(color, ImDrawFlags_None, stroke * 1.2F);
        const ImVec2 tip = p(std::cos(arcEnd) * 0.32F, std::sin(arcEnd) * 0.32F);
        drawArrowHead(drawList, tip, -std::sin(arcEnd), std::cos(arcEnd), size * 0.2F, color);
        break;
    }
    case Icon::Folder:
        drawList->AddRectFilled(p(-0.44F, -0.32F), p(-0.04F, -0.12F), color, size * 0.06F);
        drawList->AddRectFilled(p(-0.44F, -0.2F), p(0.44F, 0.34F), color, size * 0.08F);
        break;
    case Icon::Import:
        drawList->AddLine(p(0.0F, -0.42F), p(0.0F, 0.08F), color, stroke * 1.3F);
        drawArrowHead(drawList, p(0.0F, 0.16F), 0.0F, 1.0F, size * 0.24F, color);
        drawList->AddLine(p(-0.42F, 0.12F), p(-0.42F, 0.4F), color, stroke * 1.2F);
        drawList->AddLine(p(-0.42F, 0.4F), p(0.42F, 0.4F), color, stroke * 1.2F);
        drawList->AddLine(p(0.42F, 0.4F), p(0.42F, 0.12F), color, stroke * 1.2F);
        break;
    case Icon::Archive:
        drawList->AddRect(p(-0.4F, -0.36F), p(0.4F, -0.14F), color, size * 0.04F, 0, stroke);
        drawList->AddRect(p(-0.34F, -0.14F), p(0.34F, 0.4F), color, size * 0.04F, 0, stroke);
        drawList->AddLine(p(-0.1F, 0.02F), p(0.1F, 0.02F), color, stroke * 1.3F);
        break;
    case Icon::Grid:
        for (const float x : {-0.38F, 0.06F})
            for (const float y : {-0.38F, 0.06F})
                drawList->AddRectFilled(p(x, y), p(x + 0.32F, y + 0.32F), color, size * 0.05F);
        break;
    case Icon::List:
        for (const float y : {-0.3F, 0.0F, 0.3F}) {
            drawList->AddRectFilled(p(-0.42F, y - 0.06F), p(-0.3F, y + 0.06F), color);
            drawList->AddLine(p(-0.18F, y), p(0.42F, y), color, stroke * 1.2F);
        }
        break;
    case Icon::Sidebar:
        drawList->AddRect(p(-0.42F, -0.36F), p(0.42F, 0.36F), color, size * 0.06F, 0, stroke);
        drawList->AddRectFilled(p(0.12F, -0.36F), p(0.42F, 0.36F), color, size * 0.06F, ImDrawFlags_RoundCornersRight);
        break;
    case Icon::Trash:
        drawList->AddLine(p(-0.4F, -0.28F), p(0.4F, -0.28F), color, stroke * 1.2F);
        drawList->AddLine(p(-0.12F, -0.4F), p(0.12F, -0.4F), color, stroke * 1.2F);
        drawList->AddQuad(p(-0.3F, -0.2F), p(0.3F, -0.2F), p(0.24F, 0.42F), p(-0.24F, 0.42F), color, stroke);
        drawList->AddLine(p(-0.08F, -0.06F), p(-0.08F, 0.28F), color, stroke);
        drawList->AddLine(p(0.08F, -0.06F), p(0.08F, 0.28F), color, stroke);
        break;
    case Icon::Image:
        drawList->AddRect(p(-0.44F, -0.36F), p(0.44F, 0.36F), color, size * 0.06F, 0, stroke);
        drawList->AddTriangleFilled(p(-0.34F, 0.28F), p(-0.08F, -0.06F), p(0.16F, 0.28F), color);
        drawList->AddTriangleFilled(p(0.04F, 0.28F), p(0.2F, 0.06F), p(0.36F, 0.28F), color);
        drawList->AddCircleFilled(p(0.2F, -0.16F), size * 0.08F, color, 10);
        break;
    case Icon::Code:
        drawList->AddLine(p(-0.16F, -0.3F), p(-0.42F, 0.0F), color, stroke * 1.3F);
        drawList->AddLine(p(-0.42F, 0.0F), p(-0.16F, 0.3F), color, stroke * 1.3F);
        drawList->AddLine(p(0.16F, -0.3F), p(0.42F, 0.0F), color, stroke * 1.3F);
        drawList->AddLine(p(0.42F, 0.0F), p(0.16F, 0.3F), color, stroke * 1.3F);
        drawList->AddLine(p(0.08F, -0.38F), p(-0.08F, 0.38F), color, stroke);
        break;
    case Icon::Audio:
        drawList->AddLine(p(-0.06F, 0.26F), p(-0.06F, -0.38F), color, stroke * 1.2F);
        drawList->AddLine(p(-0.06F, -0.38F), p(0.3F, -0.26F), color, stroke * 1.4F);
        drawList->AddCircleFilled(p(-0.2F, 0.28F), size * 0.15F, color, 12);
        break;
    case Icon::File: {
        const std::array page{p(-0.32F, -0.44F), p(0.12F, -0.44F), p(0.32F, -0.24F), p(0.32F, 0.44F), p(-0.32F, 0.44F)};
        drawList->AddPolyline(page.data(), static_cast<int>(page.size()), color, ImDrawFlags_Closed, stroke);
        drawList->AddLine(p(0.12F, -0.44F), p(0.12F, -0.24F), color, stroke);
        drawList->AddLine(p(0.12F, -0.24F), p(0.32F, -0.24F), color, stroke);
        break;
    }
    case Icon::ChevronRight:
        drawList->AddLine(p(-0.1F, -0.26F), p(0.14F, 0.0F), color, stroke * 1.2F);
        drawList->AddLine(p(0.14F, 0.0F), p(-0.1F, 0.26F), color, stroke * 1.2F);
        break;
    case Icon::Save:
        drawList->AddRect(p(-0.4F, -0.4F), p(0.4F, 0.4F), color, size * 0.06F, 0, stroke);
        drawList->AddRectFilled(p(-0.22F, -0.4F), p(0.18F, -0.12F), color);
        drawList->AddRectFilled(p(-0.24F, 0.08F), p(0.24F, 0.4F), color, size * 0.03F);
        break;
    case Icon::Brush:
        drawList->AddLine(p(0.4F, -0.4F), p(-0.02F, 0.04F), color, stroke * 1.6F);
        drawList->AddCircleFilled(p(-0.18F, 0.2F), size * 0.15F, color, 12);
        drawList->AddTriangleFilled(p(-0.32F, 0.26F), p(-0.12F, 0.34F), p(-0.44F, 0.44F), color);
        break;
    case Icon::Leaf: {
        constexpr int segments = 10;
        std::array<ImVec2, segments * 2> leaf{};
        for (int index = 0; index < segments; ++index) {
            const float t = static_cast<float>(index) / static_cast<float>(segments - 1);
            const float bulge = std::sin(t * kPi) * 0.22F;
            const float x = -0.34F + t * 0.72F;
            const float y = 0.34F - t * 0.72F;
            leaf[static_cast<std::size_t>(index)] = p(x - bulge, y - bulge);
            leaf[static_cast<std::size_t>(segments * 2 - 1 - index)] = p(x + bulge, y + bulge);
        }
        drawList->AddConvexPolyFilled(leaf.data(), static_cast<int>(leaf.size()), color);
        drawList->AddLine(p(-0.44F, 0.44F), p(-0.2F, 0.2F), color, stroke * 1.2F);
        break;
    }
    }
}

bool iconButton(const char* id, const Icon icon, const char* tooltip, const IconButtonOptions& options) {
    const float frameHeight = ImGui::GetFrameHeight();
    const ImVec2 size{options.size.x > 0.0F ? options.size.x : frameHeight,
                      options.size.y > 0.0F ? options.size.y : frameHeight};
    const ImGuiButtonFlags flags = options.noNavFocus ? ImGuiButtonFlags_NoNavFocus : ImGuiButtonFlags_None;
    const bool clicked = ImGui::InvisibleButton(id, size, flags);
    const bool hovered = ImGui::IsItemHovered();
    const bool held = ImGui::IsItemActive();
    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    const Palette& palette = colors();
    const float rounding = ImGui::GetStyle().FrameRounding;

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    ImVec4 background{0.0F, 0.0F, 0.0F, 0.0F};
    if (options.overlay)
        background = palette.overlay;
    if (options.active)
        background = palette.accentMuted;
    if (hovered)
        background = options.active ? withAlpha(palette.accent, 0.42F) : palette.controlHover;
    if (held)
        background = withAlpha(palette.accent, 0.55F);
    if (background.w > 0.0F)
        drawList->AddRectFilled(min, max, ImGui::GetColorU32(background), rounding);
    if (options.active)
        drawList->AddRect(min, max, ImGui::GetColorU32(withAlpha(palette.accent, 0.85F)), rounding);
    else if (options.overlay)
        drawList->AddRect(min, max, ImGui::GetColorU32(palette.overlayBorder), rounding);

    ImVec4 iconColor = options.tint     ? *options.tint
                       : options.active ? palette.accentHover
                       : hovered        ? palette.textPrimary
                                        : (options.overlay ? palette.textPrimary : palette.textSecondary);
    const float iconSize = std::min(size.x, size.y) * 0.56F;
    drawIcon(drawList, icon, {std::round((min.x + max.x) * 0.5F), std::round((min.y + max.y) * 0.5F)}, iconSize,
             ImGui::GetColorU32(iconColor));

    if (tooltip != nullptr && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort | ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", tooltip);
    return clicked;
}

void iconLabel(const Icon icon, const ImVec4 color, const float size) {
    const float extent = size > 0.0F ? size : ImGui::GetFontSize();
    const float lineHeight = std::max(extent, ImGui::GetTextLineHeight());
    ImGui::Dummy({extent, lineHeight});
    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    drawIcon(ImGui::GetWindowDrawList(), icon, {std::round((min.x + max.x) * 0.5F), std::round((min.y + max.y) * 0.5F)},
             extent * 0.86F, ImGui::GetColorU32(color));
}

Icon entityIcon(const Engine::ScenePreset& scene, const Engine::Entity entity) {
    const auto view = scene.view();
    if (!view.valid(entity))
        return Icon::GameObject;
    if (view.has<Engine::CameraComponent>(entity))
        return Icon::Camera;
    if (view.has<Engine::LightComponent>(entity))
        return Icon::Light;
    if (view.has<Engine::TerrainComponent>(entity))
        return Icon::Terrain;
    if (view.has<Engine::WaterBodyComponent>(entity))
        return Icon::Water;
    if (view.has<Engine::ProceduralCloudComponent>(entity))
        return Icon::Cloud;
    if (view.has<Engine::ParticleEmitterComponent>(entity) || view.has<Engine::SmokeEmitterComponent>(entity))
        return Icon::Particles;
    if (view.has<Engine::MeshRenderer>(entity))
        return Icon::Mesh;
    return Icon::GameObject;
}

ImVec4 entityIconColor(const Icon icon) {
    const Palette& palette = colors();
    switch (icon) {
    case Icon::Camera:
        return palette.iconCamera;
    case Icon::Light:
        return palette.iconLight;
    case Icon::Terrain:
        return palette.iconTerrain;
    case Icon::Water:
    case Icon::Cloud:
        return palette.iconWater;
    case Icon::Particles:
        return palette.warning;
    case Icon::Mesh:
        return palette.iconMesh;
    default:
        return palette.textSecondary;
    }
}

} // namespace EditorUI

// NOLINTEND(readability-magic-numbers)
