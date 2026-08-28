int drawSceneOrientationGizmo(const ImVec2 imageMin, const ImVec2 imageMax,
                              const float yawDegrees, const float pitchDegrees,
                              bool& consumesClick) {
    constexpr float radius = EditorConstants::forty;
    constexpr float axisLength = EditorConstants::thirtyTwo;
    const float yaw = yawDegrees * EditorConstants::radiansPerDegree;
    const float pitch = pitchDegrees * EditorConstants::radiansPerDegree;

    // Express world axes in the Scene View camera's screen-space basis.
    const float right[3]{-std::sin(yaw), 0.0F, std::cos(yaw)};
    const float up[3]{
        -std::cos(yaw) * std::sin(pitch), std::cos(pitch),
        -std::sin(yaw) * std::sin(pitch)
    };
    const ImVec2 center{
        imageMax.x - radius - EditorConstants::twelve,
        imageMax.y - radius - EditorConstants::twelve
    };
    const ImU32 colors[3]{
        IM_COL32(255, 45, 45, 255), IM_COL32(36, 245, 79, 255),
        IM_COL32(45, 135, 255, 255)
    };
    constexpr const char *labels[3]{"X", "Y", "Z"};

    ImDrawList *drawList = ImGui::GetWindowDrawList();
    int hoveredAxis = -1;
    const bool mouseInsideImage = ImGui::IsMouseHoveringRect(imageMin, imageMax);
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    for (int axis = 0; axis < 3; ++axis) {
        const ImVec2 end{
            center.x + right[axis] * axisLength,
            center.y - up[axis] * axisLength
        };
        const ImVec2 direction{end.x - center.x, end.y - center.y};
        const ImVec2 offset{mouse.x - center.x, mouse.y - center.y};
        const float lengthSquared = (direction.x * direction.x) + (direction.y * direction.y);
        const float projection = std::clamp(
            (offset.x * direction.x + (offset.y * direction.y)) / lengthSquared, 0.0F, 1.0F);
        const ImVec2 closest{center.x + (direction.x * projection), center.y + (direction.y * projection)};
        const float distanceX = mouse.x - closest.x;
        const float distanceY = mouse.y - closest.y;
        if (mouseInsideImage && (distanceX * distanceX) + (distanceY * distanceY) <= 100.0F) {
            hoveredAxis = axis;
        }
        drawList->AddLine(center, end, colors[axis], 4.0F);
        drawList->AddCircleFilled(end, 5.0F, colors[axis]);
        drawList->AddText({end.x + 5.0F, end.y - 7.0F}, colors[axis], labels[axis]);
    }
    drawList->AddCircleFilled(center, 4.0F, IM_COL32(255, 255, 255, 255));
    const float centerDistance = std::hypot(mouse.x - center.x, mouse.y - center.y);
    if (mouseInsideImage && centerDistance <= 10.0F && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        // The centre is a visual part of the navigation gizmo.  It has no
        // orientation action, but it must still not fall through to scene
        // picking and clear the current object selection.
        consumesClick = true;
    }
    if (hoveredAxis >= 0) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            consumesClick = true;
            return hoveredAxis;
        }
    }
    return -1;
}
struct ViewportInteraction final {
    bool cameraInput{};
    bool sceneClicked{};
    bool terrainGeometryChanged{};
    Engine::Entity createdEntity{Engine::NullEntity};
    float normalizedX{};
    float normalizedY{};
};

Engine::Camera sceneViewCamera(const Engine::Renderer& renderer, const ImVec2 min,
                               const ImVec2 max) {
    Engine::Camera camera{
        Engine::Degrees{EditorConstants::cameraFieldOfView},
        (max.x - min.x) / std::max(max.y - min.y, EditorConstants::one),
        EditorConstants::cameraNearPlane, EditorConstants::cameraFarPlane
    };
    camera.setPosition(renderer.editorCameraPosition());
    camera.setRotation(Engine::Degrees{renderer.editorCameraYaw()},
                       Engine::Degrees{renderer.editorCameraPitch()});
    return camera;
}

float dotProduct(const Engine::Vec3 &lhs, const Engine::Vec3 &rhs) {
    return lhs.x() * rhs.x() + lhs.y() * rhs.y() + lhs.z() * rhs.z();
}

ImVec2 projectGizmoPoint(const Engine::Camera &camera, const Engine::Vec3 &point,
                         const ImVec2 min, const ImVec2 max) {
    // Use the renderer's view/projection path instead of duplicating its
    // camera math here. This keeps the overlay locked to geometry while the
    // Scene View camera moves or rotates.
    const glm::vec4 clip = camera.projectionMatrix().native() * camera.viewMatrix().native() *
                           glm::vec4{point.native(), 1.0F};
    if (clip.w <= EditorConstants::minimumDepth) return {-10000.0F, -10000.0F};
    const float ndcX = clip.x / clip.w;
    const float ndcY = clip.y / clip.w;
    return {
        min.x + (ndcX + EditorConstants::one) * (max.x - min.x) * EditorConstants::half,
        min.y + (ndcY + EditorConstants::one) * (max.y - min.y) * EditorConstants::half
    };
}

float distanceToLineSegment(const ImVec2 point, const ImVec2 start, const ImVec2 end) {
    const ImVec2 direction{end.x - start.x, end.y - start.y};
    const ImVec2 offset{point.x - start.x, point.y - start.y};
    const float lengthSquared = (direction.x * direction.x) + (direction.y * direction.y);
    const float t = std::clamp(((offset.x * direction.x) + (offset.y * direction.y)) /
                               std::max(lengthSquared, EditorConstants::one), EditorConstants::zero,
                               EditorConstants::one);
    const ImVec2 closest{start.x + (direction.x * t), start.y + (direction.y * t)};
    return std::hypot(point.x - closest.x, point.y - closest.y);
}

Engine::Vec3 viewportRayDirection(const Engine::Camera &camera, const ImVec2 mouse,
                                  const ImVec2 min, const ImVec2 max) {
    constexpr float halfFovTangent = EditorConstants::halfFovTangent;
    const float aspect = (max.x - min.x) / std::max(max.y - min.y, EditorConstants::one);
    const float normalizedX = ((mouse.x - min.x) / (max.x - min.x)) * EditorConstants::two - EditorConstants::one;
    const float normalizedY = ((mouse.y - min.y) / (max.y - min.y)) * EditorConstants::two - EditorConstants::one;
    return (camera.forward() + camera.right() * (normalizedX * aspect * halfFovTangent) -
            camera.up() * (normalizedY * halfFovTangent)).normalized();
}

bool intersectRayPlane(const Engine::Vec3 &rayOrigin, const Engine::Vec3 &rayDirection,
                       const Engine::Vec3 &planeOrigin, const Engine::Vec3 &planeNormal,
                       Engine::Vec3 &intersection) {
    const float denominator = dotProduct(rayDirection, planeNormal);
    if (std::abs(denominator) < EditorConstants::epsilon) {
        return false;
    }
    const float distance = dotProduct(planeOrigin - rayOrigin, planeNormal) / denominator;
    if (distance < 0.0F) {
        return false;
    }
    intersection = rayOrigin + rayDirection * distance;
    return true;
}

float gizmoWorldSize(const Engine::Camera &camera, const Engine::Vec3 &origin,
                     const ImVec2 min, const ImVec2 max) {
    constexpr float halfFovTangent = EditorConstants::halfFovTangent;
    constexpr float desiredPixels = EditorConstants::gizmoDesiredPixels;
    const float depth = std::max(dotProduct(origin - camera.position(), camera.forward()), 0.1F);
    return std::clamp(depth * EditorConstants::two * halfFovTangent * desiredPixels /
                      std::max(max.y - min.y, EditorConstants::one),
                      EditorConstants::gizmoMinimumSize, EditorConstants::gizmoMaximumSize);
}

void drawProjectedCameraLine(ImDrawList *drawList, const Engine::Camera &viewCamera,
                             const Engine::Vec3 &start, const Engine::Vec3 &end,
                             const ImVec2 min, const ImVec2 max, const ImU32 color,
                             const float thickness) {
    const ImVec2 projectedStart = projectGizmoPoint(viewCamera, start, min, max);
    const ImVec2 projectedEnd = projectGizmoPoint(viewCamera, end, min, max);
    if (projectedStart.x < -9000.0F || projectedEnd.x < -9000.0F) return;
    drawList->AddLine(projectedStart, projectedEnd, color, thickness);
}

void drawCameraGizmos(const Engine::ScenePreset &scene, const Engine::Entity selected,
                      const Engine::Renderer &renderer, const ImVec2 min, const ImVec2 max) {
    const Engine::Camera viewCamera = sceneViewCamera(renderer, min, max);
    ImDrawList *drawList = ImGui::GetWindowDrawList();
    scene.editor().view<Engine::CameraComponent, Engine::Transform>(
        [&](const Engine::Entity entity, const Engine::CameraComponent &component,
            const Engine::Transform &transform) {
            // Use the same orientation convention as the runtime camera:
            // Transform X is pitch and Y is yaw.
            Engine::Camera camera{
                Engine::Degrees{component.isPerspective() ? component.fieldOfView : 45.0F},
                std::max(component.aspectRatio, 0.01F),
                std::max(component.nearClip, 0.0001F),
                std::max(component.farClip, component.nearClip + 0.001F)
            };
            camera.setPosition(transform.position);
            camera.setRotation(Engine::Degrees{transform.rotation.y()},
                               Engine::Degrees{transform.rotation.x()});

            const float size = gizmoWorldSize(viewCamera, transform.position, min, max) * 0.72F;
            const Engine::Vec3 forward = camera.forward();
            const Engine::Vec3 right = camera.right();
            const Engine::Vec3 up = camera.up();
            const Engine::Vec3 position = transform.position;
            const Engine::Vec3 bodyCenter = position - forward * (size * 0.08F);
            const Engine::Vec3 bodyFront = bodyCenter + forward * (size * 0.22F);
            const Engine::Vec3 bodyBack = bodyCenter - forward * (size * 0.22F);
            const float bodyHalfWidth = size * 0.25F;
            const float bodyHalfHeight = size * 0.17F;
            const Engine::Vec3 bodyCorners[8]{
                bodyBack - right * bodyHalfWidth - up * bodyHalfHeight,
                bodyBack + right * bodyHalfWidth - up * bodyHalfHeight,
                bodyBack + right * bodyHalfWidth + up * bodyHalfHeight,
                bodyBack - right * bodyHalfWidth + up * bodyHalfHeight,
                bodyFront - right * bodyHalfWidth - up * bodyHalfHeight,
                bodyFront + right * bodyHalfWidth - up * bodyHalfHeight,
                bodyFront + right * bodyHalfWidth + up * bodyHalfHeight,
                bodyFront - right * bodyHalfWidth + up * bodyHalfHeight,
            };

            const ImU32 color = entity == selected ? IM_COL32(80, 230, 235, 255)
                                                    : IM_COL32(245, 190, 75, 235);
            const float thickness = entity == selected ? 2.5F : 1.8F;
            constexpr int bodyEdges[12][2]{
                {0, 1}, {1, 2}, {2, 3}, {3, 0}, {4, 5}, {5, 6},
                {6, 7}, {7, 4}, {0, 4}, {1, 5}, {2, 6}, {3, 7}
            };
            for (const auto &edge : bodyEdges) {
                drawProjectedCameraLine(drawList, viewCamera, bodyCorners[edge[0]],
                                         bodyCorners[edge[1]], min, max, color, thickness);
            }

            // The pyramid is the camera's visible direction and makes the
            // icon useful even when the camera body is viewed edge-on.
            const float frustumDepth = size * 1.35F;
            const float halfHeight = component.isPerspective()
                                         ? std::tan(component.fieldOfView *
                                                    EditorConstants::radiansPerDegree * 0.5F) * frustumDepth
                                         : size * 0.42F;
            const float halfWidth = halfHeight * std::max(component.aspectRatio, 0.01F);
            const Engine::Vec3 frustumCenter = position + forward * frustumDepth;
            const Engine::Vec3 frustumCorners[4]{
                frustumCenter - right * halfWidth - up * halfHeight,
                frustumCenter + right * halfWidth - up * halfHeight,
                frustumCenter + right * halfWidth + up * halfHeight,
                frustumCenter - right * halfWidth + up * halfHeight
            };
            for (const Engine::Vec3 &corner : frustumCorners) {
                drawProjectedCameraLine(drawList, viewCamera, position, corner,
                                         min, max, color, thickness);
            }
            for (int corner = 0; corner < 4; ++corner) {
                drawProjectedCameraLine(drawList, viewCamera, frustumCorners[corner],
                                         frustumCorners[(corner + 1) % 4], min, max, color, thickness);
            }

            const ImVec2 labelPosition = projectGizmoPoint(viewCamera, position, min, max);
            if (labelPosition.x >= min.x && labelPosition.x <= max.x &&
                labelPosition.y >= min.y && labelPosition.y <= max.y) {
                drawList->AddText({labelPosition.x + 8.0F, labelPosition.y - 8.0F},
                                  color, entityName(scene, entity));
            }
        });
}

void drawLightGizmos(const Engine::ScenePreset &scene, const Engine::Entity selected,
                     const Engine::Renderer &renderer, const ImVec2 min, const ImVec2 max) {
    const Engine::Camera viewCamera = sceneViewCamera(renderer, min, max);
    ImDrawList *drawList = ImGui::GetWindowDrawList();
    scene.editor().view<Engine::LightComponent, Engine::Transform>(
        [&](const Engine::Entity entity, const Engine::LightComponent &light,
            const Engine::Transform &transform) {
            const ImVec2 center = projectGizmoPoint(viewCamera, transform.position, min, max);
            if (center.x < min.x || center.x > max.x || center.y < min.y || center.y > max.y) return;

            const bool selectedLight = entity == selected;
            const ImU32 color = !light.enabled ? IM_COL32(125, 125, 125, 210)
                                : selectedLight ? IM_COL32(80, 230, 235, 255)
                                                : IM_COL32(255, 210, 70, 255);
            const float radius = selectedLight ? 9.0F : 7.0F;
            constexpr int rayCount = 8;
            for (int ray = 0; ray < rayCount; ++ray) {
                const float angle = static_cast<float>(ray) * 2.0F * 3.14159265F / rayCount;
                const ImVec2 start{center.x + std::cos(angle) * (radius + 2.0F),
                                   center.y + std::sin(angle) * (radius + 2.0F)};
                const ImVec2 end{center.x + std::cos(angle) * (radius + 6.0F),
                                 center.y + std::sin(angle) * (radius + 6.0F)};
                drawList->AddLine(start, end, color, selectedLight ? 2.5F : 2.0F);
            }
            drawList->AddCircleFilled(center, radius, color);
            drawList->AddCircle(center, radius, IM_COL32(255, 255, 255, 220), 0, 1.2F);

            // Directional lights illuminate along their local -Z axis.  The
            // arrow makes the light's orientation readable in Scene View.
            const glm::vec3 rawDirection = glm::vec3(transform.matrix().native() *
                                                       glm::vec4{0.0F, 0.0F, -1.0F, 0.0F});
            if (glm::length(rawDirection) > 1e-6F) {
                const Engine::Vec3 direction{glm::normalize(rawDirection)};
                const float size = gizmoWorldSize(viewCamera, transform.position, min, max);
                const ImVec2 tip = projectGizmoPoint(viewCamera,
                    transform.position + direction * (size * 2.0F), min, max);
                if (tip.x >= min.x && tip.x <= max.x && tip.y >= min.y && tip.y <= max.y) {
                    drawList->AddLine(center, tip, color, selectedLight ? 2.5F : 2.0F);
                    const float angle = std::atan2(tip.y - center.y, tip.x - center.x);
                    constexpr float arrowLength = 8.0F;
                    const ImVec2 left{tip.x - std::cos(angle - 0.55F) * arrowLength,
                                      tip.y - std::sin(angle - 0.55F) * arrowLength};
                    const ImVec2 right{tip.x - std::cos(angle + 0.55F) * arrowLength,
                                       tip.y - std::sin(angle + 0.55F) * arrowLength};
                    drawList->AddTriangleFilled(tip, left, right, color);
                }
            }
            drawList->AddText({center.x + radius + 8.0F, center.y - radius}, color,
                              entityName(scene, entity));
        });
}

bool drawTranslationGizmo(Engine::ScenePreset &scene, const Engine::Entity selected,
                          const Engine::Renderer &renderer, const ImVec2 min, const ImVec2 max) {
    if (selected == Engine::NullEntity || !scene.editor().valid(selected) ||
        !scene.editor().has<Engine::Transform>(selected))
        return false;

    Engine::Camera camera{
        Engine::Degrees{EditorConstants::cameraFieldOfView},
        (max.x - min.x) / (max.y - min.y),
        EditorConstants::cameraNearPlane, EditorConstants::cameraFarPlane
    };
    camera.setPosition(renderer.editorCameraPosition());
    camera.setRotation(Engine::Degrees{renderer.editorCameraYaw()},
                       Engine::Degrees{renderer.editorCameraPitch()});
    Engine::Vec3 origin = renderer.editorGizmoPosition(selected);
    float gizmoSize = gizmoWorldSize(camera, origin, min, max);
    const Engine::Vec3 axes[3]{{1.0F, 0.0F, 0.0F}, {0.0F, 1.0F, 0.0F}, {0.0F, 0.0F, 1.0F}};
    const ImU32 colors[3]{
        IM_COL32(235, 70, 70, 255), IM_COL32(70, 235, 100, 255),
        IM_COL32(70, 130, 245, 255)
    };
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    ImDrawList *drawList = ImGui::GetWindowDrawList();
    ImVec2 originScreen = projectGizmoPoint(camera, origin, min, max);
    const bool hoveringOrigin = ImGui::IsMouseHoveringRect(min, max) &&
        std::hypot(mouse.x - originScreen.x, mouse.y - originScreen.y) <= 14.0F;
    int hoveredAxis = -1;
    ImVec2 axisEnds[3]{};
    for (int axis = 0; axis < 3; ++axis) {
        axisEnds[axis] = projectGizmoPoint(camera, origin + axes[axis] * gizmoSize, min, max);
        if (distanceToLineSegment(mouse, originScreen, axisEnds[axis]) < EditorConstants::hitTestRadius &&
            ImGui::IsMouseHoveringRect(min, max))
            hoveredAxis = axis;
    }

    struct DragState final {
        Engine::Entity entity{Engine::NullEntity};
        int axis{-1};
        Engine::Vec3 startPosition{};
        ImVec2 startMouse{};
        ImVec2 startAxisDirection{};
        float startAxisLength{};
        float startWorldSize{};
    };
    static DragState drag;
    bool dragging = false;
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && hoveredAxis >= 0) {
        const ImVec2 axisDirection{
            axisEnds[hoveredAxis].x - originScreen.x,
            axisEnds[hoveredAxis].y - originScreen.y,
        };
        drag = {
            selected, hoveredAxis,
            scene.editor().get<Engine::Transform>(selected).position,
            mouse, axisDirection,
            std::max(std::hypot(axisDirection.x, axisDirection.y), 1.0F), gizmoSize
        };
    }
    if (drag.entity == selected && drag.axis >= 0 && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        const ImVec2 delta{mouse.x - drag.startMouse.x, mouse.y - drag.startMouse.y};
        const float pixels = (delta.x * drag.startAxisDirection.x +
                              delta.y * drag.startAxisDirection.y) / drag.startAxisLength;
        float worldDistance = pixels * (drag.startWorldSize / drag.startAxisLength);
        if (ImGui::GetIO().KeyCtrl) {
            worldDistance = std::round(worldDistance / 0.25F) * 0.25F;
        }
        scene.edit(selected).setPosition(drag.startPosition + axes[drag.axis] * worldDistance);
        // The scene image is rendered later in this frame. Re-project the
        // overlay from the updated transform so it does not trail the object
        // by one frame while dragging.
        origin = renderer.editorGizmoPosition(selected);
        gizmoSize = gizmoWorldSize(camera, origin, min, max);
        originScreen = projectGizmoPoint(camera, origin, min, max);
        for (int axis = 0; axis < 3; ++axis) {
            axisEnds[axis] = projectGizmoPoint(camera, origin + axes[axis] * gizmoSize, min, max);
        }
        dragging = true;
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    }
    if (drag.axis >= 0 && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) drag = {};
    for (int axis = 0; axis < 3; ++axis) {
        drawList->AddLine(originScreen, axisEnds[axis], colors[axis], hoveredAxis == axis ? 8.0F : 5.0F);
        drawList->AddCircleFilled(axisEnds[axis], hoveredAxis == axis ? 9.0F : 7.0F, colors[axis]);
        drawList->AddText({axisEnds[axis].x + 7.0F, axisEnds[axis].y - 8.0F}, colors[axis],
                          axis == 0 ? "X" : axis == 1 ? "Y" : "Z");
    }
    drawList->AddCircleFilled(originScreen, 8.0F, IM_COL32(245, 245, 245, 255));
    // The centre is part of the manipulator too.  Consume a click there so a
    // missed axis never deselects the object being edited.
    return dragging || hoveredAxis >= 0 || hoveringOrigin;
}

enum class GizmoMode { Translate, Rotate };

bool drawViewportGizmoTools(const ImVec2 imageMin, const ImVec2 visibleMin, GizmoMode &gizmoMode) {
    constexpr float buttonSize = 34.0F;
    constexpr float gap = 6.0F;
    const ImVec2 toolbarMin{std::max(imageMin.x + 12.0F, visibleMin.x + 12.0F),
                            std::max(imageMin.y + 12.0F, visibleMin.y + 12.0F)};
    ImDrawList *drawList = ImGui::GetWindowDrawList();
    bool consumedClick = false;

    for (int index = 0; index < 2; ++index) {
        const bool active = (index == 0 && gizmoMode == GizmoMode::Translate) ||
                            (index == 1 && gizmoMode == GizmoMode::Rotate);
        const ImVec2 min{toolbarMin.x + static_cast<float>(index) * (buttonSize + gap), toolbarMin.y};
        const ImVec2 max{min.x + buttonSize, min.y + buttonSize};

        ImGui::SetCursorScreenPos(min);
        ImGui::PushID(index);
        const bool clicked = ImGui::InvisibleButton("##gizmo-tool", {buttonSize, buttonSize});
        const bool hovered = ImGui::IsItemHovered();
        ImGui::PopID();

        const ImU32 background = ImGui::GetColorU32(active
                                                        ? ImVec4{0.06F, 0.48F, 0.59F, 0.96F}
                                                        : hovered
                                                              ? ImVec4{0.12F, 0.20F, 0.25F, 0.96F}
                                                              : ImVec4{0.07F, 0.11F, 0.15F, 0.92F});
        const ImU32 border = ImGui::GetColorU32(active
                                                    ? ImVec4{0.20F, 0.86F, 0.84F, 1.0F}
                                                    : ImVec4{0.25F, 0.34F, 0.40F, 1.0F});
        const ImU32 icon = ImGui::GetColorU32(ImVec4{0.93F, 0.97F, 0.99F, 1.0F});
        drawList->AddRectFilled(min, max, background, 6.0F);
        drawList->AddRect(min, max, border, 6.0F, 0, 1.0F);

        const ImVec2 center{(min.x + max.x) * 0.5F, (min.y + max.y) * 0.5F};
        if (index == 0) {
            constexpr float arm = 9.0F;
            constexpr float head = 4.0F;
            drawList->AddRectFilled({center.x - 3.0F, center.y - 3.0F},
                                    {center.x + 3.0F, center.y + 3.0F}, icon, 1.5F);
            drawList->AddLine({center.x - arm, center.y}, {center.x + arm, center.y}, icon, 2.0F);
            drawList->AddLine({center.x, center.y - arm}, {center.x, center.y + arm}, icon, 2.0F);
            drawList->AddTriangleFilled({center.x - arm - head, center.y},
                                        {center.x - arm + 1.0F, center.y - head},
                                        {center.x - arm + 1.0F, center.y + head}, icon);
            drawList->AddTriangleFilled({center.x + arm + head, center.y},
                                        {center.x + arm - 1.0F, center.y - head},
                                        {center.x + arm - 1.0F, center.y + head}, icon);
            drawList->AddTriangleFilled({center.x, center.y - arm - head},
                                        {center.x - head, center.y - arm + 1.0F},
                                        {center.x + head, center.y - arm + 1.0F}, icon);
            drawList->AddTriangleFilled({center.x, center.y + arm + head},
                                        {center.x - head, center.y + arm - 1.0F},
                                        {center.x + head, center.y + arm - 1.0F}, icon);
        } else {
            constexpr float arcStart = 0.55F;
            constexpr float arcEnd = 5.55F;
            constexpr int arcSegments = 20;
            ImVec2 arcPoints[arcSegments + 1];
            for (int point = 0; point <= arcSegments; ++point) {
                const float angle = arcStart + (arcEnd - arcStart) *
                    static_cast<float>(point) / static_cast<float>(arcSegments);
                arcPoints[point] = {center.x + std::cos(angle) * 9.0F,
                                    center.y + std::sin(angle) * 9.0F};
            }
            drawList->AddPolyline(arcPoints, arcSegments + 1, icon, ImDrawFlags_None, 2.0F);

            const ImVec2 tip = arcPoints[arcSegments];
            const float tangentX = -std::sin(arcEnd);
            const float tangentY = std::cos(arcEnd);
            const float normalX = std::cos(arcEnd);
            const float normalY = std::sin(arcEnd);
            drawList->AddTriangleFilled(
                {tip.x + tangentX * 3.5F, tip.y + tangentY * 3.5F},
                {tip.x - tangentX * 2.0F + normalX * 3.5F,
                 tip.y - tangentY * 2.0F + normalY * 3.5F},
                {tip.x - tangentX * 2.0F - normalX * 3.5F,
                 tip.y - tangentY * 2.0F - normalY * 3.5F}, icon);
        }

        if (hovered) ImGui::SetTooltip(index == 0 ? "Move gizmo (W)" : "Rotate gizmo (E)");
        if (clicked) {
            const GizmoMode requestedMode = index == 0 ? GizmoMode::Translate : GizmoMode::Rotate;
            gizmoMode = requestedMode;
            consumedClick = true;
        }
    }
    return consumedClick;
}

bool drawRotationGizmo(Engine::ScenePreset &scene, const Engine::Entity selected,
                       const Engine::Renderer &renderer, const ImVec2 min, const ImVec2 max) {
    if (selected == Engine::NullEntity || !scene.editor().valid(selected) ||
        !scene.editor().has<Engine::Transform>(selected)) {
        return false;
    }

    Engine::Camera camera{
        Engine::Degrees{EditorConstants::cameraFieldOfView},
        (max.x - min.x) / (max.y - min.y),
        EditorConstants::cameraNearPlane, EditorConstants::cameraFarPlane
    };
    camera.setPosition(renderer.editorCameraPosition());
    camera.setRotation(Engine::Degrees{renderer.editorCameraYaw()},
                       Engine::Degrees{renderer.editorCameraPitch()});
    const Engine::Vec3 origin = renderer.editorGizmoPosition(selected);
    const Engine::Vec3 ringBasisA[3]{{0.0F, 1.0F, 0.0F}, {1.0F, 0.0F, 0.0F}, {1.0F, 0.0F, 0.0F}};
    const Engine::Vec3 ringBasisB[3]{{0.0F, 0.0F, 1.0F}, {0.0F, 0.0F, 1.0F}, {0.0F, 1.0F, 0.0F}};
    const ImU32 colors[3]{
        IM_COL32(235, 70, 70, 255), IM_COL32(70, 235, 100, 255),
        IM_COL32(70, 130, 245, 255)
    };
    constexpr float pi = 3.14159265358979323846F;
    constexpr int segments = EditorConstants::rotationSegmentCount;
    const float radius = gizmoWorldSize(camera, origin, min, max);
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    ImDrawList *drawList = ImGui::GetWindowDrawList();
    const ImVec2 originScreen = projectGizmoPoint(camera, origin, min, max);
    const bool hoveringOrigin = ImGui::IsMouseHoveringRect(min, max) &&
        std::hypot(mouse.x - originScreen.x, mouse.y - originScreen.y) <= 14.0F;
    int hoveredAxis = -1;
    float closestRingDistance = EditorConstants::rotationHitTestRadius;
    ImVec2 ringPoints[3][segments + 1]{};
    for (int axis = 0; axis < 3; ++axis) {
        for (int segment = 0; segment <= segments; ++segment) {
            const float angle = 2.0F * pi * static_cast<float>(segment) / segments;
            const Engine::Vec3 point = origin + radius *
                                       (ringBasisA[axis] * std::cos(angle) + ringBasisB[axis] * std::sin(angle));
            ringPoints[axis][segment] = projectGizmoPoint(camera, point, min, max);
            if (segment > 0 && ImGui::IsMouseHoveringRect(min, max)) {
                const float distance = distanceToLineSegment(
                    mouse, ringPoints[axis][segment - 1], ringPoints[axis][segment]);
                if (distance < closestRingDistance) {
                    closestRingDistance = distance;
                    hoveredAxis = axis;
                }
            }
        }
    }
    for (int axis = 0; axis < 3; ++axis) {
        for (int segment = 1; segment <= segments; ++segment) {
            drawList->AddLine(ringPoints[axis][segment - 1], ringPoints[axis][segment], colors[axis],
                              hoveredAxis == axis ? EditorConstants::hoveredRotationRingThickness
                                                  : EditorConstants::rotationRingThickness);
        }
    }
    drawList->AddCircleFilled(originScreen, 7.0F, IM_COL32(245, 245, 245, 255));

    struct DragState final {
        Engine::Entity entity{Engine::NullEntity};
        int axis{-1};
        Engine::Vec3 startRotation{};
        Engine::Vec3 lastDirection{};
        ImVec2 lastScreenDirection{};
        float accumulatedRadians{};
        float screenRotationSign{1.0F};
        bool useScreenSpace{};
    };
    static DragState drag;
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && hoveredAxis >= 0) {
        const Engine::Vec3 axis = hoveredAxis == 0
                                      ? Engine::Vec3{1.0F, 0.0F, 0.0F}
                                      : hoveredAxis == 1
                                            ? Engine::Vec3{0.0F, 1.0F, 0.0F}
                                            : Engine::Vec3{0.0F, 0.0F, 1.0F};
        // A ring seen almost edge-on has a plane nearly parallel to the view
        // ray. Ray-plane rotation then becomes excessively sensitive, so use
        // its on-screen angular motion for that singular view instead.
        const bool useScreenSpace = std::abs(dotProduct(camera.forward(), axis)) < 0.15F;
        Engine::Vec3 hitPoint{};
        if (!useScreenSpace && !intersectRayPlane(camera.position(), viewportRayDirection(camera, mouse, min, max),
                                                   origin, axis, hitPoint))
            return true;
        const ImVec2 screenDirection{mouse.x - originScreen.x, mouse.y - originScreen.y};
        const ImVec2 projectedA{ringPoints[hoveredAxis][0].x - originScreen.x,
                                ringPoints[hoveredAxis][0].y - originScreen.y};
        const ImVec2 projectedB{ringPoints[hoveredAxis][segments / 4].x - originScreen.x,
                                ringPoints[hoveredAxis][segments / 4].y - originScreen.y};
        const float screenCross = projectedA.x * projectedB.y - projectedA.y * projectedB.x;
        drag = {
            selected, hoveredAxis, scene.editor().get<Engine::Transform>(selected).rotation,
            useScreenSpace ? Engine::Vec3{} : (hitPoint - origin).normalized(), screenDirection,
            0.0F, screenCross < 0.0F ? -1.0F : 1.0F, useScreenSpace
        };
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        return true;
    }
    if (drag.entity == selected && drag.axis >= 0 && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        const Engine::Vec3 axis = drag.axis == 0
                                      ? Engine::Vec3{1.0F, 0.0F, 0.0F}
                                      : drag.axis == 1
                                            ? Engine::Vec3{0.0F, 1.0F, 0.0F}
                                            : Engine::Vec3{0.0F, 0.0F, 1.0F};
        if (drag.useScreenSpace) {
            const ImVec2 currentDirection{mouse.x - originScreen.x, mouse.y - originScreen.y};
            const float currentLength = std::hypot(currentDirection.x, currentDirection.y);
            const float previousLength = std::hypot(drag.lastScreenDirection.x, drag.lastScreenDirection.y);
            if (currentLength > EditorConstants::epsilon && previousLength > EditorConstants::epsilon) {
                const float cross = drag.lastScreenDirection.x * currentDirection.y -
                                    drag.lastScreenDirection.y * currentDirection.x;
                const float dot = drag.lastScreenDirection.x * currentDirection.x +
                                  drag.lastScreenDirection.y * currentDirection.y;
                drag.accumulatedRadians += drag.screenRotationSign * std::atan2(cross, dot);
            }
            drag.lastScreenDirection = currentDirection;
        } else {
            Engine::Vec3 hitPoint{};
            if (!intersectRayPlane(camera.position(), viewportRayDirection(camera, mouse, min, max),
                                   origin, axis, hitPoint))
                return true;
            const Engine::Vec3 currentDirection = (hitPoint - origin).normalized();
            drag.accumulatedRadians += std::atan2(
                dotProduct(axis, Engine::cross(drag.lastDirection, currentDirection)),
                dotProduct(drag.lastDirection, currentDirection));
            drag.lastDirection = currentDirection;
        }
        Engine::Vec3 rotation = drag.startRotation;
        float degrees = drag.accumulatedRadians * EditorConstants::degreesPerRadian;
        if (ImGui::GetIO().KeyCtrl) {
            degrees = std::round(degrees / 15.0F) * 15.0F;
        }
        if (drag.axis == 0) {
            rotation.setX(drag.startRotation.x() + degrees);
        } else if (drag.axis == 1) {
            rotation.setY(drag.startRotation.y() + degrees);
        } else {
            rotation.setZ(drag.startRotation.z() + degrees);
        }
        scene.edit(selected).setRotation(rotation);
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        return true;
    }
    if (drag.axis >= 0 && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) drag = {};
    return hoveredAxis >= 0 || hoveringOrigin;
}

Engine::Vec3 terrainLocalPoint(const Engine::Transform& transform,
                               const Engine::Vec3& worldPoint) {
    const glm::vec4 local = glm::inverse(transform.matrix().native()) *
                            glm::vec4{worldPoint.native(), 1.0F};
    return Engine::Vec3{glm::vec3{local}};
}

Engine::Vec3 terrainWorldPoint(const Engine::Transform& transform,
                               const Engine::Vec3& localPoint) {
    const glm::vec4 world = transform.matrix().native() * glm::vec4{localPoint.native(), 1.0F};
    return Engine::Vec3{glm::vec3{world}};
}

std::optional<Engine::Vec3> raycastTerrain(const Engine::Mesh& mesh,
                                           const Engine::Transform& transform,
                                           const Engine::Vec3& origin,
                                           const Engine::Vec3& direction) {
    float nearest = EditorConstants::cameraFarPlane;
    std::optional<Engine::Vec3> point;
    for (std::size_t index = 0; index + 2 < mesh.indices.size(); index += 3) {
        const Engine::Vec3 a = terrainWorldPoint(transform, mesh.vertices[mesh.indices[index]].position);
        const Engine::Vec3 b = terrainWorldPoint(transform, mesh.vertices[mesh.indices[index + 1]].position);
        const Engine::Vec3 c = terrainWorldPoint(transform, mesh.vertices[mesh.indices[index + 2]].position);
        const Engine::Vec3 edgeAB = b - a;
        const Engine::Vec3 edgeAC = c - a;
        const Engine::Vec3 p = Engine::cross(direction, edgeAC);
        const float determinant = dotProduct(edgeAB, p);
        if (std::abs(determinant) <= EditorConstants::epsilon) continue;
        const float inverseDeterminant = 1.0F / determinant;
        const Engine::Vec3 fromA = origin - a;
        const float u = dotProduct(fromA, p) * inverseDeterminant;
        if (u < 0.0F || u > 1.0F) continue;
        const Engine::Vec3 q = Engine::cross(fromA, edgeAB);
        const float v = dotProduct(direction, q) * inverseDeterminant;
        if (v < 0.0F || u + v > 1.0F) continue;
        const float distance = dotProduct(edgeAC, q) * inverseDeterminant;
        if (distance < 0.0F || distance >= nearest) continue;
        nearest = distance;
        point = origin + direction * distance;
    }
    return point;
}

bool applyTerrainBrush(Engine::ScenePreset& scene, const Engine::Entity entity,
                       const Engine::Vec3& localPoint, TerrainSculptState& state) {
    auto terrain = scene.editor().get<Engine::TerrainComponent>(entity);
    const float deltaTime = std::clamp(static_cast<float>(Engine::Time::deltaTime()),
                                       1.0F / 240.0F, 1.0F / 20.0F);
    if (!terrain.sculpt(localPoint.x(), localPoint.z(), state.radius,
                        state.strength * deltaTime, state.mode, state.flattenHeight)) {
        return false;
    }

    auto mesh = std::make_shared<Engine::Mesh>(terrain.createMesh());
    scene.editor().modify<Engine::TerrainComponent>(entity,
        [&](auto& component) { component = std::move(terrain); });
    scene.editor().modify<Engine::MeshRenderer>(entity,
        [&](auto& renderer) { renderer.mesh = mesh; });
    if (scene.editor().has<Engine::ColliderComponent>(entity)) {
        scene.editor().modify<Engine::ColliderComponent>(entity, [&](auto& collider) {
            if (auto* meshCollider = std::get_if<Engine::MeshCollider>(&collider.shape)) {
                meshCollider->mesh = mesh;
            }
        });
    }
    return true;
}

bool drawTerrainSculpt(Engine::ScenePreset& scene, const Engine::Entity selected,
                       const Engine::Renderer& renderer,
                       const ImVec2 min, const ImVec2 max, TerrainSculptState& state,
                       const bool imageHovered, bool& geometryChanged) {
    if (!state.enabled || selected == Engine::NullEntity || !scene.editor().valid(selected) ||
        !scene.editor().has<Engine::TerrainComponent>(selected) ||
        !scene.editor().has<Engine::MeshRenderer>(selected) ||
        !scene.editor().has<Engine::Transform>(selected)) return false;

    // The Scene View image is no longer ImGui's last item here: drawing the
    // viewport gizmo toolbar creates invisible buttons after it.  Keep using
    // the hover state captured directly after ImGui::Image instead.
    const bool hovered = imageHovered;
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const Engine::Camera camera = sceneViewCamera(renderer, min, max);
    const Engine::Vec3 rayDirection = viewportRayDirection(camera, mouse, min, max);

    const auto& transform = scene.editor().get<Engine::Transform>(selected);
    const auto& terrain = scene.editor().get<Engine::TerrainComponent>(selected);
    const auto& rendererComponent = scene.editor().get<Engine::MeshRenderer>(selected);
    const auto hit = hovered && rendererComponent.hasMesh()
                         ? raycastTerrain(*rendererComponent.mesh, transform, camera.position(), rayDirection)
                             : std::nullopt;
    const bool hitsSelected = hit.has_value();
    Engine::Vec3 localHit{};
    if (hitsSelected) {
        localHit = terrainLocalPoint(transform, *hit);
        constexpr int segments = 48;
        constexpr float pi = 3.14159265358979323846F;
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        ImVec2 previous{};
        for (int segment = 0; segment <= segments; ++segment) {
            const float angle = 2.0F * pi * static_cast<float>(segment) / segments;
            const float x = localHit.x() + std::cos(angle) * state.radius;
            const float z = localHit.z() + std::sin(angle) * state.radius;
            const float y = terrain.sampleHeight(x, z) + 0.035F;
            const ImVec2 point = projectGizmoPoint(
                camera, terrainWorldPoint(transform, {x, y, z}), min, max);
            if (segment > 0) {
                drawList->AddLine(previous, point, IM_COL32(70, 220, 125, 245), 2.5F);
            }
            previous = point;
        }
        const ImVec2 center = projectGizmoPoint(camera, *hit, min, max);
        drawList->AddCircleFilled(center, 4.0F, IM_COL32(235, 250, 238, 255));
    }

    if (hovered && hitsSelected && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        state.strokeActive = true;
        state.strokeEntity = selected;
        state.flattenHeight = localHit.y();
    }
    if (state.strokeActive && state.strokeEntity == selected &&
        ImGui::IsMouseDown(ImGuiMouseButton_Left) && hitsSelected) {
        geometryChanged |= applyTerrainBrush(scene, selected, localHit, state);
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    }
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        state.strokeActive = false;
        state.strokeEntity = Engine::NullEntity;
    }
    return true;
}

ViewportInteraction drawViewport(Engine::ScenePreset &scene, Engine::Assets::Content& content,
                                 const Engine::Entity selected, Engine::Renderer &renderer,
                                 Engine::ViewportHandle gameDescriptor,
                                 Engine::ViewportHandle sceneDescriptor,
                                 const float sceneCameraYaw, const float sceneCameraPitch,
                                 bool &showGameView, GizmoMode &gizmoMode,
                                 TerrainSculptState& terrainSculpt, const bool playing, bool& isOpen) {
    static std::string assetDropError;
    if (playing) {
        showGameView = true;
    }
    const Engine::ViewportHandle descriptor = showGameView ? gameDescriptor : sceneDescriptor;

    ImGui::Begin("Viewport", &isOpen, ImGuiWindowFlags_NoScrollbar);
    if (!playing) {
        ImGui::TextDisabled("Navigate: RMB + WASD/QE  |  Shift: faster  |  MMB: pan  |  Wheel: zoom");
        const bool terrainSelected = selected != Engine::NullEntity && scene.editor().valid(selected) &&
                                     scene.editor().has<Engine::TerrainComponent>(selected);
        if (terrainSelected) {
            ImGui::SameLine(0.0F, 4.0F);
            if (drawToolbarToggle(" Sculpt ", terrainSculpt.enabled)) {
                terrainSculpt.enabled = !terrainSculpt.enabled;
            }
            if (terrainSculpt.enabled) {
                ImGui::SameLine(0.0F, 6.0F);
                constexpr const char* modes[]{"Raise", "Lower", "Smooth", "Flatten"};
                int mode = static_cast<int>(terrainSculpt.mode);
                ImGui::SetNextItemWidth(92.0F);
                if (ImGui::Combo("##terrain-mode", &mode, modes, 4)) {
                    terrainSculpt.mode = static_cast<Engine::TerrainSculptMode>(mode);
                }
                ImGui::SameLine(0.0F, 6.0F);
                ImGui::SetNextItemWidth(105.0F);
                ImGui::SliderFloat("Radius##terrain", &terrainSculpt.radius, 0.25F, 8.0F, "R %.1f");
                ImGui::SameLine(0.0F, 6.0F);
                ImGui::SetNextItemWidth(105.0F);
                ImGui::SliderFloat("Strength##terrain", &terrainSculpt.strength, 0.1F, 12.0F, "S %.1f");
            }
        } else {
            terrainSculpt.enabled = false;
            terrainSculpt.strokeActive = false;
        }
        ImGui::SameLine(0.0F, 8.0F);
        if (EditorButton(showGameView ? " Scene View " : " Game View ").drawSmall()) {
            showGameView = !showGameView;
        }
        ImGui::Spacing();
    }
    bool viewportHovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
    const ImVec2 size = ImGui::GetContentRegionAvail();
    ViewportInteraction interaction{};
    if (descriptor && size.x > 1.0F && size.y > 1.0F) {
        constexpr float viewportAspect = EditorConstants::viewportWidthRatio /
                                         EditorConstants::viewportHeightRatio;
        // Keep the rendered view at a fixed aspect ratio. The child clips the
        // image when the panel is wider than 16:9, so the excess is removed
        // symmetrically from the top and bottom instead of distorting it.
        ImGui::PushStyleColor(ImGuiCol_ChildBg, {0.055F, 0.058F, 0.072F, 1.0F});
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 6.0F);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0.0F, 0.0F});
        ImGui::BeginChild("##viewport-frame", size, true,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        const ImVec2 frameSize = ImGui::GetContentRegionAvail();
        const float imageHeight = frameSize.x / viewportAspect;
        const float verticalOffset = (frameSize.y - imageHeight) * 0.5F;
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + verticalOffset);
        ImGui::Image(ImTextureRef{static_cast<ImTextureID>(descriptor.value)},
                     {frameSize.x, imageHeight}, {0, 0}, {1, 1});
        const ImVec2 imageMin = ImGui::GetItemRectMin();
        const ImVec2 imageMax = ImGui::GetItemRectMax();
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* payload =
                    ImGui::AcceptDragDropPayload(Editor::AssetDragDrop::modelPayload)) {
                try {
                    Engine::Vec3 position{};
                    if (!showGameView) {
                        const Engine::Camera camera = sceneViewCamera(renderer, imageMin, imageMax);
                        const Engine::Vec3 direction = viewportRayDirection(
                            camera, ImGui::GetIO().MousePos, imageMin, imageMax);
                        if (!intersectRayPlane(camera.position(), direction, Engine::Vec3{},
                                               Engine::Vec3{0.0F, 1.0F, 0.0F}, position)) {
                            position = camera.position() + direction * 5.0F;
                        }
                    }
                    interaction.createdEntity = Editor::AssetDragDrop::instantiateModel(
                        scene, content, Editor::AssetDragDrop::modelPath(*payload), position);
                    assetDropError.clear();
                } catch (const std::exception& exception) {
                    assetDropError = exception.what();
                }
            }
            ImGui::EndDragDropTarget();
        }
        const bool imageHovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
        if (imageHovered && !assetDropError.empty()) {
            ImGui::SetTooltip("Could not add model: %s", assetDropError.c_str());
        }
        viewportHovered = imageHovered;
        int gizmoAction = -1;
        bool gizmoToolsConsumeClick = false;
        bool orientationGizmoConsumesClick = false;
        if (!showGameView && !playing) {
            gizmoToolsConsumeClick = drawViewportGizmoTools(imageMin, ImGui::GetWindowPos(), gizmoMode);
            // A gizmo-tool button belongs to the currently selected object.
            // Reapply the renderer selection explicitly, so changing tools
            // cannot make its outline/focus disappear even if ImGui moves
            // keyboard focus to the toolbar button.
            if (gizmoToolsConsumeClick && selected != Engine::NullEntity &&
                scene.editor().valid(selected)) {
                renderer.setEditorSelection(selected);
            }
            gizmoAction = drawSceneOrientationGizmo(imageMin, imageMax,
                                                    sceneCameraYaw, sceneCameraPitch,
                                                    orientationGizmoConsumesClick);
            switch (gizmoAction) {
                case 0: renderer.setEditorCameraRotation(180.0F, 0.0F);
                    break; // +X view
                case 1: renderer.setEditorCameraRotation(0.0F, -89.0F);
                    break; // +Y view
                case 2: renderer.setEditorCameraRotation(-90.0F, 0.0F);
                    break; // +Z view
                default: break;
            }
            drawCameraGizmos(scene, selected, renderer, imageMin, imageMax);
            drawLightGizmos(scene, selected, renderer, imageMin, imageMax);
        }
        const bool sculptConsumesClick = gizmoAction < 0 && !showGameView && !playing &&
            drawTerrainSculpt(scene, selected, renderer, imageMin, imageMax, terrainSculpt,
                              imageHovered, interaction.terrainGeometryChanged);
        const bool gizmoConsumesClick = gizmoToolsConsumeClick || orientationGizmoConsumesClick ||
            sculptConsumesClick ||
            (gizmoAction < 0 && !showGameView && !playing &&
                                        (gizmoMode == GizmoMode::Translate
                                             ? drawTranslationGizmo(scene, selected, renderer, imageMin, imageMax)
                                             : drawRotationGizmo(scene, selected, renderer, imageMin, imageMax)));
        if (!showGameView && !playing && gizmoAction < 0 && !gizmoConsumesClick && imageHovered &&
            ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            const ImVec2 mouse = ImGui::GetIO().MousePos;
            interaction.sceneClicked = true;
            interaction.normalizedX = ((mouse.x - imageMin.x) / (imageMax.x - imageMin.x)) * 2.0F - 1.0F;
            interaction.normalizedY = ((mouse.y - imageMin.y) / (imageMax.y - imageMin.y)) * 2.0F - 1.0F;
        }
        ImGui::EndChild();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor();
    }
    ImGui::End();
    // Do not enable camera navigation just because a mouse button is held
    // elsewhere in the editor. The previous global button check captured the
    // cursor after right- or middle-clicking menus and side panels, leaving
    // ImGui unable to receive subsequent clicks.
    interaction.cameraInput = !playing && !showGameView && viewportHovered;
    return interaction;
}

Engine::Entity pickSceneEntity(Engine::ScenePreset &scene, Engine::PhysicsSystem &physics,
                               const Engine::Renderer &renderer, const float normalizedX,
                               const float normalizedY, const float aspect) {
    Engine::Camera camera{
        Engine::Degrees{EditorConstants::cameraFieldOfView}, aspect,
        EditorConstants::cameraNearPlane, EditorConstants::cameraFarPlane
    };
    camera.setPosition(renderer.editorCameraPosition());
    camera.setRotation(Engine::Degrees{renderer.editorCameraYaw()},
                       Engine::Degrees{renderer.editorCameraPitch()});
    constexpr float pi = 3.14159265358979323846F;
    const float scale = std::tan(30.0F * pi / 180.0F);
    const Engine::Vec3 direction = (camera.forward() +
                                    camera.right() * (normalizedX * aspect * scale) -
                                    camera.up() * (normalizedY * scale)).normalized();
    if (const auto hit = physics.raycast(scene, camera.position(), direction)) {
        return scene.findEntity(hit->actor.id());
    }
    return Engine::NullEntity;
}
