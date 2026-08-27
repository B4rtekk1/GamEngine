int drawSceneOrientationGizmo(const ImVec2 imageMin, const ImVec2 imageMax,
                              const float yawDegrees, const float pitchDegrees) {
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
    if (hoveredAxis >= 0) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            return hoveredAxis;
        }
    }
    return -1;
}

struct ViewportInteraction final {
    bool cameraInput{};
    bool sceneClicked{};
    float normalizedX{};
    float normalizedY{};
};

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
    return dragging || hoveredAxis >= 0;
}

enum class GizmoMode { Translate, Rotate };

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
    return hoveredAxis >= 0;
}

ViewportInteraction drawViewport(Engine::ScenePreset &scene, const Engine::Entity selected,
                                 Engine::Renderer &renderer, Engine::ViewportHandle gameDescriptor,
                                 Engine::ViewportHandle sceneDescriptor,
                                 const float sceneCameraYaw, const float sceneCameraPitch,
                                 bool &showGameView, GizmoMode &gizmoMode, const bool playing) {
    if (playing) {
        showGameView = true;
    }
    const Engine::ViewportHandle descriptor = showGameView ? gameDescriptor : sceneDescriptor;

    ImGui::Begin("Viewport", nullptr, ImGuiWindowFlags_NoScrollbar);
    drawPanelHeader("Viewport", playing ? "Playing · Game Camera" : showGameView ? "Game Camera" : "Scene Camera");
    if (!playing) {
        if (drawToolbarToggle(" Move ", gizmoMode == GizmoMode::Translate)) {
            gizmoMode = GizmoMode::Translate;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Translate gizmo (W)");
        }
        ImGui::SameLine(0.0F, 4.0F);
        if (drawToolbarToggle(" Rotate ", gizmoMode == GizmoMode::Rotate)) {
            gizmoMode = GizmoMode::Rotate;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Rotate gizmo (E)");
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
        viewportHovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
        const bool gizmoConsumesClick = !showGameView && !playing &&
                                        (gizmoMode == GizmoMode::Translate
                                             ? drawTranslationGizmo(scene, selected, renderer, ImGui::GetItemRectMin(),
                                                                    ImGui::GetItemRectMax())
                                             : drawRotationGizmo(scene, selected, renderer, ImGui::GetItemRectMin(),
                                                                 ImGui::GetItemRectMax()));
        int gizmoAction = -1;
        if (!showGameView && !playing) {
            gizmoAction = drawSceneOrientationGizmo(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(),
                                                    sceneCameraYaw, sceneCameraPitch);
            switch (gizmoAction) {
                case 0: renderer.setEditorCameraRotation(180.0F, 0.0F);
                    break; // +X view
                case 1: renderer.setEditorCameraRotation(0.0F, -89.0F);
                    break; // +Y view
                case 2: renderer.setEditorCameraRotation(-90.0F, 0.0F);
                    break; // +Z view
                default: break;
            }
        }
        if (!showGameView && !playing && gizmoAction < 0 && !gizmoConsumesClick && ImGui::IsItemHovered() &&
            ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            const ImVec2 min = ImGui::GetItemRectMin();
            const ImVec2 max = ImGui::GetItemRectMax();
            const ImVec2 mouse = ImGui::GetIO().MousePos;
            interaction.sceneClicked = true;
            interaction.normalizedX = ((mouse.x - min.x) / (max.x - min.x)) * 2.0F - 1.0F;
            interaction.normalizedY = ((mouse.y - min.y) / (max.y - min.y)) * 2.0F - 1.0F;
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

