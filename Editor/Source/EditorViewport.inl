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
    bool gameCameraInput{};
    bool sceneClicked{};
    bool terrainGeometryChanged{};
    bool terrainGrassChanged{};
    std::uint32_t terrainFirstVertex{};
    std::uint32_t terrainVertexCount{std::numeric_limits<std::uint32_t>::max()};
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

std::optional<Engine::AABB> meshWorldBounds(const Engine::MeshRenderer &renderer,
                                            const Engine::Transform &transform) {
    if (!renderer.hasMesh()) return std::nullopt;

    const auto &vertices = renderer.mesh->vertices;
    Engine::AABB localBounds{
        .min = vertices.front().position,
        .max = vertices.front().position,
    };
    for (const Engine::Vertex &vertex : vertices) {
        localBounds.min.setX(std::min(localBounds.min.x(), vertex.position.x()));
        localBounds.min.setY(std::min(localBounds.min.y(), vertex.position.y()));
        localBounds.min.setZ(std::min(localBounds.min.z(), vertex.position.z()));
        localBounds.max.setX(std::max(localBounds.max.x(), vertex.position.x()));
        localBounds.max.setY(std::max(localBounds.max.y(), vertex.position.y()));
        localBounds.max.setZ(std::max(localBounds.max.z(), vertex.position.z()));
    }
    return localBounds.transformed(transform.matrix().native());
}

bool boundsOverlapOnOtherAxes(const Engine::AABB &lhs, const Engine::AABB &rhs,
                              const int movementAxis) {
    for (int axis = 0; axis < EditorConstants::axisCount; ++axis) {
        if (axis == movementAxis) continue;
        const float lhsMin = axis == 0 ? lhs.min.x() : axis == 1 ? lhs.min.y() : lhs.min.z();
        const float lhsMax = axis == 0 ? lhs.max.x() : axis == 1 ? lhs.max.y() : lhs.max.z();
        const float rhsMin = axis == 0 ? rhs.min.x() : axis == 1 ? rhs.min.y() : rhs.min.z();
        const float rhsMax = axis == 0 ? rhs.max.x() : axis == 1 ? rhs.max.y() : rhs.max.z();
        if (lhsMax < rhsMin || rhsMax < lhsMin) return false;
    }
    return true;
}

std::optional<Engine::Vec3> snapMeshVerticesTogether(
    const Engine::MeshRenderer &movingRenderer, const Engine::Transform &movingTransform,
    const Engine::MeshRenderer &targetRenderer, const Engine::Transform &targetTransform) {
    if (!movingRenderer.hasMesh() || !targetRenderer.hasMesh()) return std::nullopt;

    // Most editor primitives (including fence segments) have relatively few
    // vertices.  Sampling large imported meshes keeps Ctrl-drag responsive,
    // while still retaining all vertices of the common low-poly editor assets.
    constexpr std::size_t maximumSamplesPerMesh = 128;
    const auto &movingVertices = movingRenderer.mesh->vertices;
    const auto &targetVertices = targetRenderer.mesh->vertices;
    const std::size_t movingStep = std::max<std::size_t>(
        1, movingVertices.size() / maximumSamplesPerMesh);
    const std::size_t targetStep = std::max<std::size_t>(
        1, targetVertices.size() / maximumSamplesPerMesh);
    const glm::mat4 movingMatrix = movingTransform.matrix().native();
    const glm::mat4 targetMatrix = targetTransform.matrix().native();

    float shortestDistance = EditorConstants::snapTranslation;
    std::optional<Engine::Vec3> bestOffset;
    for (std::size_t movingIndex = 0; movingIndex < movingVertices.size();
         movingIndex += movingStep) {
        const Engine::Vec3 movingPoint{movingMatrix * glm::vec4{
            movingVertices[movingIndex].position.native(), 1.0F}};
        for (std::size_t targetIndex = 0; targetIndex < targetVertices.size();
             targetIndex += targetStep) {
            const Engine::Vec3 targetPoint{targetMatrix * glm::vec4{
                targetVertices[targetIndex].position.native(), 1.0F}};
            const Engine::Vec3 offset = targetPoint - movingPoint;
            const float distance = offset.length();
            if (distance > EditorConstants::epsilon && distance <= shortestDistance) {
                shortestDistance = distance;
                bestOffset = offset;
            }
        }
    }
    return bestOffset;
}

Engine::Vec3 snapTranslationToObjects(Engine::ScenePreset &scene, const Engine::Entity selected,
                                      const Engine::Vec3 &candidatePosition, const int axis) {
    const auto &selectedRenderer = scene.editor().get<Engine::MeshRenderer>(selected);
    Engine::Transform candidateTransform = scene.editor().get<Engine::Transform>(selected);
    candidateTransform.position = candidatePosition;
    const auto candidateBounds = meshWorldBounds(selectedRenderer, candidateTransform);
    if (!candidateBounds) return candidatePosition;

    float closestOffset = EditorConstants::snapTranslation;
    bool foundSnap = false;
    float closestVertexDistance = EditorConstants::snapTranslation;
    std::optional<Engine::Vec3> vertexOffset;
    scene.editor().view<Engine::MeshRenderer, Engine::Transform>(
        [&](const Engine::Entity entity, const Engine::MeshRenderer &renderer,
            const Engine::Transform &transform) {
            if (entity == selected) return;
            const auto targetBounds = meshWorldBounds(renderer, transform);
            if (!targetBounds) return;

            // Aligning real mesh vertices lets the ends of angled elements,
            // such as diagonal fence arms, meet exactly.  AABB-only snapping
            // cannot represent those endpoints after rotation.
            if (const auto offset = snapMeshVerticesTogether(
                    selectedRenderer, candidateTransform, renderer, transform);
                offset && offset->length() < closestVertexDistance) {
                closestVertexDistance = offset->length();
                vertexOffset = offset;
            }

            if (!boundsOverlapOnOtherAxes(*candidateBounds, *targetBounds, axis)) return;

            const float movingMin = axis == 0 ? candidateBounds->min.x()
                                  : axis == 1 ? candidateBounds->min.y() : candidateBounds->min.z();
            const float movingMax = axis == 0 ? candidateBounds->max.x()
                                  : axis == 1 ? candidateBounds->max.y() : candidateBounds->max.z();
            const float targetMin = axis == 0 ? targetBounds->min.x()
                                  : axis == 1 ? targetBounds->min.y() : targetBounds->min.z();
            const float targetMax = axis == 0 ? targetBounds->max.x()
                                  : axis == 1 ? targetBounds->max.y() : targetBounds->max.z();
            for (const float offset : {targetMin - movingMax, targetMax - movingMin}) {
                if (std::abs(offset) <= EditorConstants::snapTranslation &&
                    (!foundSnap || std::abs(offset) < std::abs(closestOffset))) {
                    closestOffset = offset;
                    foundSnap = true;
                }
            }
        });
    if (vertexOffset) return candidatePosition + *vertexOffset;
    if (!foundSnap) return candidatePosition;

    Engine::Vec3 snapped = candidatePosition;
    if (axis == 0) snapped.setX(snapped.x() + closestOffset);
    else if (axis == 1) snapped.setY(snapped.y() + closestOffset);
    else snapped.setZ(snapped.z() + closestOffset);
    return snapped;
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
        Engine::Vec3 position = drag.startPosition + axes[drag.axis] * worldDistance;
        if (ImGui::GetIO().KeyCtrl && scene.editor().has<Engine::MeshRenderer>(selected)) {
            position = snapTranslationToObjects(scene, selected, position, drag.axis);
        }
        scene.edit(selected).setPosition(position);
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
        // These are viewport overlays, not editor controls.  In particular,
        // do not let a tool switch take ImGui navigation focus away from the
        // selected object in the Scene View.
        const bool clicked = ImGui::InvisibleButton(
            "##gizmo-tool", {buttonSize, buttonSize}, ImGuiButtonFlags_NoNavFocus);
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

std::optional<Engine::Vec3> raycastTerrain(const Engine::TerrainComponent& terrain,
                                           const Engine::Transform& transform,
                                           const Engine::Vec3& origin,
                                           const Engine::Vec3& direction) {
    const glm::mat4 inverse = glm::inverse(transform.matrix().native());
    const Engine::Vec3 localOrigin{glm::vec3{inverse * glm::vec4{origin.native(), 1.0F}}};
    const Engine::Vec3 localDirection{glm::vec3{inverse * glm::vec4{direction.native(), 0.0F}}};
    float entry = 0.0F;
    float exit = EditorConstants::cameraFarPlane;
    const auto clipAxis = [&](const float rayOrigin, const float rayDirection,
                              const float minimum, const float maximum) {
        if (std::abs(rayDirection) <= EditorConstants::epsilon) {
            return rayOrigin >= minimum && rayOrigin <= maximum;
        }
        float near = (minimum - rayOrigin) / rayDirection;
        float far = (maximum - rayOrigin) / rayDirection;
        if (near > far) std::swap(near, far);
        entry = std::max(entry, near);
        exit = std::min(exit, far);
        return entry <= exit;
    };
    if (!clipAxis(localOrigin.x(), localDirection.x(), -terrain.width * 0.5F, terrain.width * 0.5F) ||
        !clipAxis(localOrigin.z(), localDirection.z(), -terrain.depth * 0.5F, terrain.depth * 0.5F) ||
        !clipAxis(localOrigin.y(), localDirection.y(), terrain.minimumHeight, terrain.maximumHeight)) {
        return std::nullopt;
    }
    const float horizontalSpeed = std::hypot(localDirection.x(), localDirection.z());
    const float sampleSpacing = std::min(terrain.width, terrain.depth) /
                                static_cast<float>(terrain.resolution - 1);
    const float step = horizontalSpeed > EditorConstants::epsilon
                           ? std::max(sampleSpacing * 0.35F / horizontalSpeed, 1.0e-4F)
                           : std::max((exit - entry) / 256.0F, 1.0e-4F);
    const auto signedHeight = [&](const float distance) {
        const Engine::Vec3 point = localOrigin + localDirection * distance;
        return point.y() - terrain.sampleHeight(point.x(), point.z());
    };
    float previousDistance = entry;
    float previousHeight = signedHeight(entry);
    for (float distance = std::min(entry + step, exit); distance <= exit; distance = std::min(distance + step, exit)) {
        const float currentHeight = signedHeight(distance);
        if (previousHeight >= 0.0F && currentHeight <= 0.0F) {
            float low = previousDistance;
            float high = distance;
            for (int iteration = 0; iteration < 10; ++iteration) {
                const float middle = (low + high) * 0.5F;
                if (signedHeight(middle) > 0.0F) low = middle;
                else high = middle;
            }
            return terrainWorldPoint(transform, localOrigin + localDirection * ((low + high) * 0.5F));
        }
        if (distance >= exit) break;
        previousDistance = distance;
        previousHeight = currentHeight;
    }
    return std::nullopt;
}

bool applyTerrainBrush(Engine::ScenePreset& scene, const Engine::Entity entity,
                       const Engine::Vec3& localPoint, const float amount,
                       TerrainSculptState& state, Engine::TerrainRegion& dirty) {
    bool changed = false;
    scene.editor().modify<Engine::TerrainComponent>(entity, [&](auto& terrain) {
        changed = terrain.sculpt(localPoint.x(), localPoint.z(), state.radius, amount,
                                 state.mode, state.flattenHeight, state.falloff, &dirty);
        if (changed && state.workingMesh) terrain.updateMeshRegion(*state.workingMesh, dirty);
    });
    return changed;
}

bool applyTerrainPaintBrush(Engine::ScenePreset& scene, const Engine::Entity entity,
                            const Engine::Vec3& localPoint, TerrainSculptState& state,
                            Engine::TerrainRegion& dirty) {
    bool changed = false;
    scene.editor().modify<Engine::TerrainComponent>(entity, [&](auto& terrain) {
        changed = terrain.paint(localPoint.x(), localPoint.z(), state.radius,
                                state.paintLayer == 0 ? Engine::Vec3{1.0F, 0.0F, 0.0F} :
                                state.paintLayer == 1 ? Engine::Vec3{0.0F, 1.0F, 0.0F} :
                                state.paintLayer == 2 ? Engine::Vec3{0.0F, 0.0F, 1.0F} : Engine::Vec3{},
                                state.paintOpacity, state.falloff, &dirty);
        if (changed && state.workingMesh) terrain.updateMeshRegion(*state.workingMesh, dirty);
    });
    return changed;
}

void finishTerrainStroke(Engine::ScenePreset& scene, TerrainSculptState& state) {
    if (state.strokeEntity != Engine::NullEntity && state.workingMesh &&
        scene.editor().valid(state.strokeEntity) &&
        scene.editor().has<Engine::ColliderComponent>(state.strokeEntity)) {
        scene.editor().modify<Engine::ColliderComponent>(state.strokeEntity, [&](auto& collider) {
            if (auto* meshCollider = std::get_if<Engine::MeshCollider>(&collider.shape)) {
                meshCollider->mesh = state.workingMesh;
            }
        });
    }
    if (state.strokeDirty.valid) {
        if (scene.editor().valid(state.strokeEntity) &&
            scene.editor().has<Engine::TerrainComponent>(state.strokeEntity) &&
            scene.editor().has<Engine::TerrainGrassComponent>(state.strokeEntity)) {
            const auto& terrain = scene.editor().get<Engine::TerrainComponent>(state.strokeEntity);
            scene.editor().modify<Engine::TerrainGrassComponent>(state.strokeEntity, [&](auto& grass) {
                for (auto& instance : grass.instances)
                    instance.position.setY(terrain.sampleHeight(instance.position.x(), instance.position.z()));
                grass.allInstancesDirty = true;
            });
        }
        state.strokeCompleted = true;
        state.completedEntity = state.strokeEntity;
        state.completedDirty = state.strokeDirty;
    } else {
        state.heightsBeforeStroke.clear();
    }
    state.strokeActive = false;
    state.strokeEntity = Engine::NullEntity;
    state.hasPreviousPoint = false;
    state.workingMesh.reset();
    state.strokeDirty = {};
}

bool drawTerrainSculpt(Engine::ScenePreset& scene, const Engine::Entity selected,
                       const Engine::Renderer& renderer,
                       const ImVec2 min, const ImVec2 max, TerrainSculptState& state,
                       const bool imageHovered, ViewportInteraction& interaction) {
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
                         ? raycastTerrain(terrain, transform, camera.position(), rayDirection)
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
        state.hasPreviousPoint = false;
        state.workingMesh = std::make_shared<Engine::Mesh>(*rendererComponent.mesh);
        state.heightsBeforeStroke = terrain.heights;
        state.strokeDirty = {};
        scene.editor().modify<Engine::MeshRenderer>(selected, [&](auto& component) {
            component.mesh = state.workingMesh;
        });
    }
    if (state.strokeActive && state.strokeEntity == selected &&
        ImGui::IsMouseDown(ImGuiMouseButton_Left) && hitsSelected) {
        const ImGuiIO& io = ImGui::GetIO();
        if (io.KeyCtrl) state.flattenHeight = localHit.y();
        Engine::TerrainSculptMode strokeMode = state.mode;
        if (io.KeyShift && strokeMode == Engine::TerrainSculptMode::Raise) strokeMode = Engine::TerrainSculptMode::Lower;
        else if (io.KeyShift && strokeMode == Engine::TerrainSculptMode::Lower) strokeMode = Engine::TerrainSculptMode::Raise;
        const Engine::TerrainSculptMode configuredMode = state.mode;
        state.mode = strokeMode;
        const float deltaTime = std::clamp(static_cast<float>(Engine::Time::deltaTime()),
                                           1.0F / 240.0F, 1.0F / 20.0F);
        const float distance = state.hasPreviousPoint ? (localHit - state.previousPoint).length() : 0.0F;
        const float interval = std::max(state.radius * state.spacing, 0.01F);
        const int sampleCount = state.hasPreviousPoint
                                    ? std::max(1, static_cast<int>(std::ceil(distance / interval))) : 1;
        for (int sample = 1; sample <= sampleCount; ++sample) {
            const float t = static_cast<float>(sample) / static_cast<float>(sampleCount);
            const Engine::Vec3 point = state.hasPreviousPoint
                                           ? state.previousPoint + (localHit - state.previousPoint) * t : localHit;
            Engine::TerrainRegion dirty;
            if (!applyTerrainBrush(scene, selected, point, state.strength * deltaTime /
                                   static_cast<float>(sampleCount), state, dirty)) continue;
            if (!state.strokeDirty.valid) state.strokeDirty = dirty;
            else {
                state.strokeDirty.minimumX = std::min(state.strokeDirty.minimumX, dirty.minimumX);
                state.strokeDirty.minimumZ = std::min(state.strokeDirty.minimumZ, dirty.minimumZ);
                state.strokeDirty.maximumX = std::max(state.strokeDirty.maximumX, dirty.maximumX);
                state.strokeDirty.maximumZ = std::max(state.strokeDirty.maximumZ, dirty.maximumZ);
            }
            interaction.terrainGeometryChanged = true;
            const auto& changedTerrain = scene.editor().get<Engine::TerrainComponent>(selected);
            const std::uint32_t minX = dirty.minimumX == 0 ? 0 : dirty.minimumX - 1;
            const std::uint32_t minZ = dirty.minimumZ == 0 ? 0 : dirty.minimumZ - 1;
            const std::uint32_t maxX = std::min(dirty.maximumX + 1, changedTerrain.resolution - 1);
            const std::uint32_t maxZ = std::min(dirty.maximumZ + 1, changedTerrain.resolution - 1);
            const std::uint32_t first = minZ * changedTerrain.resolution + minX;
            const std::uint32_t last = maxZ * changedTerrain.resolution + maxX;
            if (interaction.terrainVertexCount == std::numeric_limits<std::uint32_t>::max()) {
                interaction.terrainFirstVertex = first;
                interaction.terrainVertexCount = last - first + 1;
            } else {
                const std::uint32_t combinedFirst = std::min(interaction.terrainFirstVertex, first);
                const std::uint32_t combinedLast = std::max(
                    interaction.terrainFirstVertex + interaction.terrainVertexCount - 1, last);
                interaction.terrainFirstVertex = combinedFirst;
                interaction.terrainVertexCount = combinedLast - combinedFirst + 1;
            }
        }
        state.mode = configuredMode;
        state.previousPoint = localHit;
        state.hasPreviousPoint = true;
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    }
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        finishTerrainStroke(scene, state);
        if (state.strokeCompleted && scene.editor().has<Engine::TerrainGrassComponent>(selected))
            interaction.terrainGrassChanged = true;
    }
    return true;
}

bool drawTerrainPaint(Engine::ScenePreset& scene, const Engine::Entity selected,
                      const Engine::Renderer& renderer, const ImVec2 min, const ImVec2 max,
                      TerrainSculptState& state, const bool imageHovered,
                      ViewportInteraction& interaction) {
    if (!state.paintEnabled || selected == Engine::NullEntity || !scene.editor().valid(selected) ||
        !scene.editor().has<Engine::TerrainComponent>(selected) ||
        !scene.editor().has<Engine::MeshRenderer>(selected) || !scene.editor().has<Engine::Transform>(selected)) return false;
    const auto& terrain = scene.editor().get<Engine::TerrainComponent>(selected);
    const auto& transform = scene.editor().get<Engine::Transform>(selected);
    const auto& meshRenderer = scene.editor().get<Engine::MeshRenderer>(selected);
    const Engine::Camera camera = sceneViewCamera(renderer, min, max);
    const auto hit = imageHovered && meshRenderer.hasMesh() ? raycastTerrain(terrain, transform, camera.position(),
        viewportRayDirection(camera, ImGui::GetIO().MousePos, min, max)) : std::nullopt;
    if (hit) {
        const Engine::Vec3 localHit = terrainLocalPoint(transform, *hit);
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        constexpr float pi = 3.14159265358979323846F;
        ImVec2 previous{};
        for (int i = 0; i <= 48; ++i) {
            const float angle = 2.0F * pi * static_cast<float>(i) / 48.0F;
            const float x = localHit.x() + std::cos(angle) * state.radius;
            const float z = localHit.z() + std::sin(angle) * state.radius;
            const ImVec2 point = projectGizmoPoint(camera, terrainWorldPoint(transform,
                {x, terrain.sampleHeight(x, z) + 0.04F, z}), min, max);
            if (i > 0) drawList->AddLine(previous, point, IM_COL32(245, 180, 65, 245), 2.5F);
            previous = point;
        }
    }
    if (imageHovered && hit && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        state.strokeActive = true; state.strokeEntity = selected; state.hasPreviousPoint = false;
        state.workingMesh = std::make_shared<Engine::Mesh>(*meshRenderer.mesh);
        state.heightsBeforeStroke = terrain.heights; state.strokeDirty = {};
        scene.editor().modify<Engine::MeshRenderer>(selected, [&](auto& component) { component.mesh = state.workingMesh; });
    }
    if (state.strokeActive && state.strokeEntity == selected && hit && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        const Engine::Vec3 localHit = terrainLocalPoint(transform, *hit);
        const float distance = state.hasPreviousPoint ? (localHit - state.previousPoint).length() : 0.0F;
        const int count = state.hasPreviousPoint ? std::max(1, static_cast<int>(std::ceil(distance /
            std::max(state.radius * state.spacing, 0.01F)))) : 1;
        for (int i = 1; i <= count; ++i) {
            const float t = static_cast<float>(i) / static_cast<float>(count);
            const Engine::Vec3 point = state.hasPreviousPoint ? state.previousPoint + (localHit - state.previousPoint) * t : localHit;
            Engine::TerrainRegion dirty;
            if (!applyTerrainPaintBrush(scene, selected, point, state, dirty)) continue;
            if (!state.strokeDirty.valid) state.strokeDirty = dirty;
            else { state.strokeDirty.minimumX = std::min(state.strokeDirty.minimumX, dirty.minimumX); state.strokeDirty.minimumZ = std::min(state.strokeDirty.minimumZ, dirty.minimumZ); state.strokeDirty.maximumX = std::max(state.strokeDirty.maximumX, dirty.maximumX); state.strokeDirty.maximumZ = std::max(state.strokeDirty.maximumZ, dirty.maximumZ); }
            interaction.terrainGeometryChanged = true;
        }
        state.previousPoint = localHit; state.hasPreviousPoint = true;
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    }
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) finishTerrainStroke(scene, state);
    return true;
}

bool drawTerrainGrass(Engine::ScenePreset& scene, const Engine::Entity selected,
                      const Engine::Renderer& renderer, const ImVec2 min, const ImVec2 max,
                      TerrainSculptState& state, const bool imageHovered,
                      ViewportInteraction& interaction) {
    if (!state.grassEnabled || selected == Engine::NullEntity || !scene.editor().valid(selected) ||
        !scene.editor().has<Engine::TerrainComponent>(selected) ||
        !scene.editor().has<Engine::Transform>(selected)) return false;

    const auto& terrain = scene.editor().get<Engine::TerrainComponent>(selected);
    const auto& transform = scene.editor().get<Engine::Transform>(selected);
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const Engine::Camera camera = sceneViewCamera(renderer, min, max);
    const auto hit = imageHovered
        ? raycastTerrain(terrain, transform, camera.position(), viewportRayDirection(camera, mouse, min, max))
        : std::nullopt;
    Engine::Vec3 localHit{};
    if (hit) {
        localHit = terrainLocalPoint(transform, *hit);
        constexpr int segments = 48;
        constexpr float pi = 3.14159265358979323846F;
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        ImVec2 previous{};
        for (int segment = 0; segment <= segments; ++segment) {
            const float angle = 2.0F * pi * static_cast<float>(segment) / segments;
            const float x = localHit.x() + std::cos(angle) * state.radius;
            const float z = localHit.z() + std::sin(angle) * state.radius;
            const ImVec2 point = projectGizmoPoint(camera, terrainWorldPoint(
                transform, {x, terrain.sampleHeight(x, z) + 0.04F, z}), min, max);
            if (segment > 0) drawList->AddLine(previous, point,
                state.grassErase ? IM_COL32(240, 90, 90, 245) : IM_COL32(80, 225, 115, 245), 2.5F);
            previous = point;
        }
    }

    if (imageHovered && hit && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        state.grassStrokeActive = true;
        state.grassStrokeChanged = false;
        state.grassHasPreviousPoint = false;
    }
    if (state.grassStrokeActive && hit && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        const float interval = std::max(state.radius * 0.65F, 0.1F);
        const float distance = state.grassHasPreviousPoint
            ? std::hypot(localHit.x() - state.grassPreviousPoint.x(),
                         localHit.z() - state.grassPreviousPoint.z()) : interval;
        if (distance >= interval || !state.grassHasPreviousPoint) {
            bool changed = false;
            if (scene.editor().has<Engine::TerrainGrassComponent>(selected)) {
                scene.editor().modify<Engine::TerrainGrassComponent>(selected, [&](auto& grass) {
                    if (state.grassErase) {
                        const auto oldSize = grass.instances.size();
                        std::erase_if(grass.instances, [&](const auto& instance) {
                            return std::hypot(instance.position.x() - localHit.x(),
                                              instance.position.z() - localHit.z()) <= state.radius;
                        });
                        changed = oldSize != grass.instances.size();
                        return;
                    }
                    if (!grass.hasPrefab() ||
                        grass.instances.size() >= Engine::TerrainGrassComponent::MaximumInstances) return;
                    constexpr float pi = 3.14159265358979323846F;
                    const std::size_t wanted = static_cast<std::size_t>(std::clamp(
                        static_cast<float>(std::ceil(state.grassDensity * pi * state.radius * state.radius)),
                        1.0F, 256.0F));
                    const std::size_t available = Engine::TerrainGrassComponent::MaximumInstances -
                                                  grass.instances.size();
                    const std::size_t count = std::min(wanted, available);
                    auto random01 = [&]() {
                        state.grassRandomState ^= state.grassRandomState << 13U;
                        state.grassRandomState ^= state.grassRandomState >> 17U;
                        state.grassRandomState ^= state.grassRandomState << 5U;
                        return static_cast<float>(state.grassRandomState & 0x00ffffffU) /
                               static_cast<float>(0x01000000U);
                    };
                    for (std::size_t i = 0; i < count; ++i) {
                        const float angle = random01() * 2.0F * pi;
                        const float radial = std::sqrt(random01()) * state.radius;
                        const float x = localHit.x() + std::cos(angle) * radial;
                        const float z = localHit.z() + std::sin(angle) * radial;
                        if (x < -terrain.width * 0.5F || x > terrain.width * 0.5F ||
                            z < -terrain.depth * 0.5F || z > terrain.depth * 0.5F) continue;
                        const float scale = state.grassMinimumScale + random01() *
                            (state.grassMaximumScale - state.grassMinimumScale);
                        grass.instances.push_back({.position = {x, terrain.sampleHeight(x, z), z},
                            .yaw = state.grassRandomYaw ? random01() * 360.0F : 0.0F,
                            .scale = scale});
                        changed = true;
                    }
                });
            }
            if (changed) state.grassStrokeChanged = true;
            state.grassPreviousPoint = localHit;
            state.grassHasPreviousPoint = true;
        }
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    }
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        if (state.grassStrokeActive && state.grassStrokeChanged)
            interaction.terrainGrassChanged = true;
        state.grassStrokeActive = false;
        state.grassStrokeChanged = false;
        state.grassHasPreviousPoint = false;
    }
    return hit.has_value();
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
        ImGui::TextDisabled(showGameView
            ? "Game camera: RMB + WASD/QE  |  Shift: faster  |  Wheel: zoom"
            : "Navigate: RMB + WASD/QE  |  Shift: faster  |  MMB: pan  |  Wheel: zoom");
        const bool terrainSelected = selected != Engine::NullEntity && scene.editor().valid(selected) &&
                                     scene.editor().has<Engine::TerrainComponent>(selected);
        if (!terrainSelected) {
            if (terrainSculpt.strokeActive) finishTerrainStroke(scene, terrainSculpt);
            terrainSculpt.enabled = false;
            terrainSculpt.paintEnabled = false;
            terrainSculpt.grassEnabled = false;
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
        const bool grassConsumesClick = gizmoAction < 0 && !showGameView && !playing &&
            drawTerrainGrass(scene, selected, renderer, imageMin, imageMax, terrainSculpt,
                             imageHovered, interaction);
        const bool sculptConsumesClick = !grassConsumesClick && gizmoAction < 0 && !showGameView && !playing &&
            drawTerrainSculpt(scene, selected, renderer, imageMin, imageMax, terrainSculpt,
                              imageHovered, interaction);
        const bool paintConsumesClick = !grassConsumesClick && !sculptConsumesClick && gizmoAction < 0 && !showGameView && !playing &&
            drawTerrainPaint(scene, selected, renderer, imageMin, imageMax, terrainSculpt, imageHovered, interaction);
        // Draw the active transform gizmo independently of the toolbar click.
        // Otherwise short-circuit evaluation below skips this call on the
        // exact frame in which the gizmo mode is changed.
        const bool transformGizmoConsumesClick = gizmoAction < 0 && !showGameView && !playing &&
            (gizmoMode == GizmoMode::Translate
                 ? drawTranslationGizmo(scene, selected, renderer, imageMin, imageMax)
                 : drawRotationGizmo(scene, selected, renderer, imageMin, imageMax));
        const bool gizmoConsumesClick = gizmoToolsConsumeClick || orientationGizmoConsumesClick ||
            sculptConsumesClick || paintConsumesClick || grassConsumesClick ||
            transformGizmoConsumesClick;
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
    interaction.gameCameraInput = showGameView && viewportHovered && !ImGui::GetIO().WantTextInput;
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
