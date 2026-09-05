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
enum class PlayModeAction { None, Start, Stop, Restart };

struct ViewportInteraction final {
    bool cameraInput{};
    bool gameCameraInput{};
    bool gameMouseCaptureRequested{};
    bool sceneClicked{};
    bool terrainGeometryChanged{};
    std::vector<Engine::Entity> terrainGeometryEntities;
    bool terrainGrassChanged{};
    std::uint32_t terrainFirstVertex{};
    std::uint32_t terrainVertexCount{std::numeric_limits<std::uint32_t>::max()};
    Engine::Entity createdEntity{Engine::NullEntity};
    std::vector<Engine::Entity> selectedEntities;
    bool selectionCommitted{};
    PlayModeAction playModeAction{PlayModeAction::None};
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
            // Keep the editor-only field-of-view indicator comfortably larger
            // than the camera body, without affecting the runtime camera FOV.
            const float frustumDepth = size * 3.0F;
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

float snapValue(const float value, const float increment) {
    return std::round(value / increment) * increment;
}

float clampedScale(const float value) {
    // A zero scale makes the model matrix singular and breaks picking,
    // normal transforms, and later object snapping.  Keep its sign so
    // intentionally mirrored meshes remain mirrored.
    if (std::abs(value) >= EditorConstants::minimumScale) return value;
    return value < 0.0F ? -EditorConstants::minimumScale : EditorConstants::minimumScale;
}

bool drawTranslationGizmo(Engine::ScenePreset &scene, const Engine::Entity selected,
                          const std::vector<Engine::Entity>& selection,
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
        std::vector<std::pair<Engine::Entity, Engine::Vec3>> startPositions;
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
            selected, hoveredAxis, {}, mouse, axisDirection,
            std::max(std::hypot(axisDirection.x, axisDirection.y), 1.0F), gizmoSize
        };
        drag.startPositions.emplace_back(selected, scene.editor().get<Engine::Transform>(selected).position);
        for (const Engine::Entity entity : selection) {
            if (entity != selected && scene.editor().valid(entity) && scene.editor().has<Engine::Transform>(entity))
                drag.startPositions.emplace_back(entity, scene.editor().get<Engine::Transform>(entity).position);
        }
    }
    if (drag.entity == selected && drag.axis >= 0 && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        const ImVec2 delta{mouse.x - drag.startMouse.x, mouse.y - drag.startMouse.y};
        const float pixels = (delta.x * drag.startAxisDirection.x +
                              delta.y * drag.startAxisDirection.y) / drag.startAxisLength;
        float worldDistance = pixels * (drag.startWorldSize / drag.startAxisLength);
        Engine::Vec3 position = drag.startPositions.front().second + axes[drag.axis] * worldDistance;
        if (ImGui::GetIO().KeyCtrl) {
            if (drag.axis == 0) position.setX(snapValue(position.x(), EditorConstants::snapTranslation));
            else if (drag.axis == 1) position.setY(snapValue(position.y(), EditorConstants::snapTranslation));
            else position.setZ(snapValue(position.z(), EditorConstants::snapTranslation));
            if (scene.editor().has<Engine::MeshRenderer>(selected)) {
                position = snapTranslationToObjects(scene, selected, position, drag.axis);
            }
        }
        const Engine::Vec3 offset = position - drag.startPositions.front().second;
        for (const auto& [entity, startPosition] : drag.startPositions)
            scene.edit(entity).setPosition(startPosition + offset);
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

bool drawScaleGizmo(Engine::ScenePreset &scene, const Engine::Entity selected,
                    const std::vector<Engine::Entity>& selection,
                    const Engine::Renderer &renderer, const ImVec2 min, const ImVec2 max) {
    if (selected == Engine::NullEntity || !scene.editor().valid(selected) ||
        !scene.editor().has<Engine::Transform>(selected)) return false;

    const Engine::Camera camera = sceneViewCamera(renderer, min, max);
    const Engine::Vec3 origin = renderer.editorGizmoPosition(selected);
    const float gizmoSize = gizmoWorldSize(camera, origin, min, max);
    const Engine::Vec3 axes[3]{{1.0F, 0.0F, 0.0F}, {0.0F, 1.0F, 0.0F}, {0.0F, 0.0F, 1.0F}};
    const ImU32 colors[3]{IM_COL32(235, 70, 70, 255), IM_COL32(70, 235, 100, 255),
                          IM_COL32(70, 130, 245, 255)};
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    ImDrawList *drawList = ImGui::GetWindowDrawList();
    const ImVec2 originScreen = projectGizmoPoint(camera, origin, min, max);
    ImVec2 axisEnds[3]{};
    int hoveredAxis = -1;
    for (int axis = 0; axis < 3; ++axis) {
        axisEnds[axis] = projectGizmoPoint(camera, origin + axes[axis] * gizmoSize, min, max);
        if (ImGui::IsMouseHoveringRect(min, max) &&
            distanceToLineSegment(mouse, originScreen, axisEnds[axis]) < EditorConstants::hitTestRadius) {
            hoveredAxis = axis;
        }
    }
    const bool hoveringUniform = ImGui::IsMouseHoveringRect(min, max) &&
        std::hypot(mouse.x - originScreen.x, mouse.y - originScreen.y) <= 12.0F;

    struct DragState final {
        Engine::Entity entity{Engine::NullEntity};
        int axis{-1}; // 0..2: axis scale; 3: uniform scale.
        std::vector<std::pair<Engine::Entity, Engine::Vec3>> startScales;
        ImVec2 startMouse{};
        ImVec2 screenDirection{};
        float screenLength{};
    };
    static DragState drag;
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && (hoveredAxis >= 0 || hoveringUniform)) {
        const int axis = hoveredAxis >= 0 ? hoveredAxis : 3;
        const ImVec2 direction = axis < 3
            ? ImVec2{axisEnds[axis].x - originScreen.x, axisEnds[axis].y - originScreen.y}
            : ImVec2{1.0F, -1.0F};
        drag = {selected, axis, {}, mouse, direction,
                std::max(std::hypot(direction.x, direction.y), 1.0F)};
        drag.startScales.emplace_back(selected, scene.editor().get<Engine::Transform>(selected).scale);
        for (const Engine::Entity entity : selection) {
            if (entity != selected && scene.editor().valid(entity) && scene.editor().has<Engine::Transform>(entity))
                drag.startScales.emplace_back(entity, scene.editor().get<Engine::Transform>(entity).scale);
        }
    }
    bool dragging = false;
    if (drag.entity == selected && drag.axis >= 0 && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        const ImVec2 delta{mouse.x - drag.startMouse.x, mouse.y - drag.startMouse.y};
        const float pixels = (delta.x * drag.screenDirection.x + delta.y * drag.screenDirection.y) /
                             drag.screenLength;
        const float factor = std::max(EditorConstants::minimumScale,
                                      EditorConstants::one + pixels / EditorConstants::gizmoDesiredPixels);
        auto scaleAxis = [&](const float value) {
            float result = value * factor;
            if (ImGui::GetIO().KeyCtrl) result = snapValue(result, EditorConstants::snapScale);
            return clampedScale(result);
        };
        for (const auto& [entity, startScale] : drag.startScales) {
            Engine::Vec3 scale = startScale;
            if (drag.axis == 3) {
                scale.setX(scaleAxis(startScale.x()));
                scale.setY(scaleAxis(startScale.y()));
                scale.setZ(scaleAxis(startScale.z()));
            } else if (drag.axis == 0) scale.setX(scaleAxis(startScale.x()));
            else if (drag.axis == 1) scale.setY(scaleAxis(startScale.y()));
            else scale.setZ(scaleAxis(startScale.z()));
            scene.edit(entity).setScale(scale);
        }
        dragging = true;
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    }
    if (drag.axis >= 0 && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) drag = {};

    for (int axis = 0; axis < 3; ++axis) {
        drawList->AddLine(originScreen, axisEnds[axis], colors[axis], hoveredAxis == axis ? 8.0F : 5.0F);
        const float cube = hoveredAxis == axis ? 8.0F : 6.0F;
        drawList->AddRectFilled({axisEnds[axis].x - cube, axisEnds[axis].y - cube},
                                {axisEnds[axis].x + cube, axisEnds[axis].y + cube}, colors[axis], 1.5F);
        drawList->AddText({axisEnds[axis].x + 8.0F, axisEnds[axis].y - 8.0F}, colors[axis],
                          axis == 0 ? "X" : axis == 1 ? "Y" : "Z");
    }
    const float centerSize = hoveringUniform ? 9.0F : 7.0F;
    drawList->AddRectFilled({originScreen.x - centerSize, originScreen.y - centerSize},
                            {originScreen.x + centerSize, originScreen.y + centerSize},
                            IM_COL32(245, 245, 245, 255), 1.5F);
    return dragging || hoveredAxis >= 0 || hoveringUniform;
}

enum class GizmoMode { Translate, Rotate, Scale };
enum class SelectionTool { Rectangle, Lasso };

bool pointInLasso(const ImVec2 point, const std::vector<ImVec2>& polygon) {
    bool inside = false;
    for (std::size_t i = 0, j = polygon.size() - 1; i < polygon.size(); j = i++) {
        const ImVec2& a = polygon[i]; const ImVec2& b = polygon[j];
        if ((a.y > point.y) != (b.y > point.y) &&
            point.x < (b.x - a.x) * (point.y - a.y) / (b.y - a.y) + a.x) inside = !inside;
    }
    return inside;
}

void drawSelectionTools(const ImVec2 imageMin, const ImVec2 visibleMin, SelectionTool& tool) {
    // The fixed-aspect image may extend above the child window when the
    // viewport is wider than its aspect ratio. Anchor overlays to the clipped,
    // visible image area so they remain available in a maximized viewport.
    const ImVec2 start{std::max(imageMin.x + 132.0F, visibleMin.x + 132.0F),
                       std::max(imageMin.y + 12.0F, visibleMin.y + 12.0F)};
    for (int index = 0; index < 2; ++index) {
        ImGui::SetCursorScreenPos({start.x + index * 72.0F, start.y});
        const bool active = (index == 0) == (tool == SelectionTool::Rectangle);
        if (active) ImGui::PushStyleColor(ImGuiCol_Button, {0.06F, 0.48F, 0.59F, 0.96F});
        if (ImGui::SmallButton(index == 0 ? "Box" : "Lasso")) tool = index == 0 ? SelectionTool::Rectangle : SelectionTool::Lasso;
        if (active) ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip(index == 0 ? "Rectangle select" : "Lasso select");
    }
}

bool drawViewportGizmoTools(const ImVec2 imageMin, const ImVec2 visibleMin, GizmoMode &gizmoMode) {
    constexpr float buttonSize = 34.0F;
    constexpr float gap = 6.0F;
    const ImVec2 toolbarMin{std::max(imageMin.x + 12.0F, visibleMin.x + 12.0F),
                            std::max(imageMin.y + 12.0F, visibleMin.y + 12.0F)};
    ImDrawList *drawList = ImGui::GetWindowDrawList();
    bool consumedClick = false;

    for (int index = 0; index < 3; ++index) {
        const bool active = (index == 0 && gizmoMode == GizmoMode::Translate) ||
                            (index == 1 && gizmoMode == GizmoMode::Rotate) ||
                            (index == 2 && gizmoMode == GizmoMode::Scale);
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
        } else if (index == 1) {
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
        } else {
            constexpr float extent = 8.0F;
            drawList->AddRect({center.x - extent, center.y - extent},
                              {center.x + extent, center.y + extent}, icon, 1.5F, 0, 2.0F);
            drawList->AddLine({center.x - 4.0F, center.y - 4.0F},
                              {center.x + 4.0F, center.y + 4.0F}, icon, 1.5F);
            drawList->AddLine({center.x + 4.0F, center.y - 4.0F},
                              {center.x - 4.0F, center.y + 4.0F}, icon, 1.5F);
        }

        if (hovered) ImGui::SetTooltip(index == 0 ? "Move gizmo (W)" :
                                       index == 1 ? "Rotate gizmo (E)" : "Scale gizmo (R)");
        if (clicked) {
            const GizmoMode requestedMode = index == 0 ? GizmoMode::Translate :
                                            index == 1 ? GizmoMode::Rotate : GizmoMode::Scale;
            gizmoMode = requestedMode;
            consumedClick = true;
        }
    }
    return consumedClick;
}

bool drawRotationGizmo(Engine::ScenePreset &scene, const Engine::Entity selected,
                       const std::vector<Engine::Entity>& selection,
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
        std::vector<std::pair<Engine::Entity, Engine::Vec3>> startRotations;
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
            selected, hoveredAxis, {},
            useScreenSpace ? Engine::Vec3{} : (hitPoint - origin).normalized(), screenDirection,
            0.0F, screenCross < 0.0F ? -1.0F : 1.0F, useScreenSpace
        };
        drag.startRotations.emplace_back(selected, scene.editor().get<Engine::Transform>(selected).rotation);
        for (const Engine::Entity entity : selection) {
            if (entity != selected && scene.editor().valid(entity) && scene.editor().has<Engine::Transform>(entity))
                drag.startRotations.emplace_back(entity, scene.editor().get<Engine::Transform>(entity).rotation);
        }
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
        float degrees = drag.accumulatedRadians * EditorConstants::degreesPerRadian;
        if (ImGui::GetIO().KeyCtrl) {
            degrees = snapValue(degrees, EditorConstants::snapRotation);
        }
        for (const auto& [entity, startRotation] : drag.startRotations) {
            Engine::Vec3 rotation = startRotation;
            if (drag.axis == 0) rotation.setX(startRotation.x() + degrees);
            else if (drag.axis == 1) rotation.setY(startRotation.y() + degrees);
            else rotation.setZ(startRotation.z() + degrees);
            scene.edit(entity).setRotation(rotation);
        }
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
                       TerrainSculptState& state, Engine::Mesh& mesh,
                       Engine::TerrainRegion& dirty) {
    bool changed = false;
    scene.editor().modify<Engine::TerrainComponent>(entity, [&](auto& terrain) {
        changed = terrain.sculpt(localPoint.x(), localPoint.z(), state.radius, amount,
                                 state.mode, state.flattenHeight, state.falloff, &dirty);
        if (changed) terrain.updateMeshRegion(mesh, dirty);
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

void stitchSculptTerrainEdges(Engine::ScenePreset& scene, TerrainSculptState& state) {
    // Adjacent tiles own separate heightmaps.  Force their coincident border
    // samples to one value after a stroke so rounding or different brush
    // sampling can never leave a visible crack between them.
    constexpr float seamTolerance = 0.001F;
    const auto edgeIndex = [](const int edge, const std::uint32_t sample,
                              const std::uint32_t resolution) -> std::size_t {
        switch (edge) {
        case 0: return static_cast<std::size_t>(sample); // z minimum
        case 1: return static_cast<std::size_t>(resolution - 1) * resolution + sample; // z maximum
        case 2: return static_cast<std::size_t>(sample) * resolution; // x minimum
        default: return static_cast<std::size_t>(sample) * resolution + resolution - 1; // x maximum
        }
    };
    const auto edgeLocalPoint = [](const Engine::TerrainComponent& terrain, const int edge,
                                   const std::uint32_t sample) {
        const float fraction = static_cast<float>(sample) / static_cast<float>(terrain.resolution - 1);
        const float x = edge < 2 ? -terrain.width * 0.5F + terrain.width * fraction
                                 : (edge == 2 ? -terrain.width : terrain.width) * 0.5F;
        const float z = edge < 2 ? (edge == 0 ? -terrain.depth : terrain.depth) * 0.5F
                                 : -terrain.depth * 0.5F + terrain.depth * fraction;
        return Engine::Vec3{x, 0.0F, z};
    };
    const auto edgePoint = [&](const Engine::TerrainComponent& terrain, const Engine::Transform& transform,
                               const int edge, const std::uint32_t sample) {
        return terrainWorldPoint(transform, edgeLocalPoint(terrain, edge, sample));
    };
    const auto sameHorizontalPoint = [&](const Engine::Vec3& lhs, const Engine::Vec3& rhs) {
        return std::hypot(lhs.x() - rhs.x(), lhs.z() - rhs.z()) <= seamTolerance;
    };

    for (std::size_t left = 0; left < state.sculptTargets.size(); ++left) {
        for (std::size_t right = left + 1; right < state.sculptTargets.size(); ++right) {
            TerrainStrokeTarget& firstTarget = state.sculptTargets[left];
            TerrainStrokeTarget& secondTarget = state.sculptTargets[right];
            if (!scene.editor().valid(firstTarget.entity) || !scene.editor().valid(secondTarget.entity)) continue;
            const auto& first = scene.editor().get<Engine::TerrainComponent>(firstTarget.entity);
            const auto& second = scene.editor().get<Engine::TerrainComponent>(secondTarget.entity);
            if (first.resolution != second.resolution) continue;
            const auto& firstTransform = scene.editor().get<Engine::Transform>(firstTarget.entity);
            const auto& secondTransform = scene.editor().get<Engine::Transform>(secondTarget.entity);
            for (int firstEdge = 0; firstEdge != 4; ++firstEdge) {
                for (int secondEdge = 0; secondEdge != 4; ++secondEdge) {
                    const std::uint32_t last = first.resolution - 1;
                    const bool sameOrder = sameHorizontalPoint(edgePoint(first, firstTransform, firstEdge, 0),
                                                               edgePoint(second, secondTransform, secondEdge, 0)) &&
                                           sameHorizontalPoint(edgePoint(first, firstTransform, firstEdge, last),
                                                               edgePoint(second, secondTransform, secondEdge, last));
                    const bool reverseOrder = sameHorizontalPoint(edgePoint(first, firstTransform, firstEdge, 0),
                                                                  edgePoint(second, secondTransform, secondEdge, last)) &&
                                              sameHorizontalPoint(edgePoint(first, firstTransform, firstEdge, last),
                                                                  edgePoint(second, secondTransform, secondEdge, 0));
                    if (!sameOrder && !reverseOrder) continue;
                    // Heights are local to each tile.  Averaging those local values
                    // works only while both transforms have exactly the same Y
                    // origin and scale; otherwise it actually creates a seam.
                    // Stitch in world space, then convert the common surface height
                    // back to each terrain's local heightmap.
                    std::vector<float> firstSharedHeights(first.resolution);
                    std::vector<float> secondSharedHeights(second.resolution);
                    for (std::uint32_t sample = 0; sample <= last; ++sample) {
                        const std::uint32_t other = reverseOrder ? last - sample : sample;
                        const Engine::Vec3 firstLocal = edgeLocalPoint(first, firstEdge, sample);
                        const Engine::Vec3 secondLocal = edgeLocalPoint(second, secondEdge, other);
                        const float firstHeight = first.heights[edgeIndex(firstEdge, sample, first.resolution)];
                        const float secondHeight = second.heights[edgeIndex(secondEdge, other, second.resolution)];
                        const float sharedWorldHeight = (terrainWorldPoint(
                            firstTransform, {firstLocal.x(), firstHeight, firstLocal.z()}).y() +
                            terrainWorldPoint(secondTransform,
                                              {secondLocal.x(), secondHeight, secondLocal.z()}).y()) * 0.5F;
                        const Engine::Vec3 seamOrigin = edgePoint(first, firstTransform, firstEdge, sample);
                        firstSharedHeights[sample] = terrainLocalPoint(
                            firstTransform, {seamOrigin.x(), sharedWorldHeight, seamOrigin.z()}).y();
                        secondSharedHeights[other] = terrainLocalPoint(
                            secondTransform, {seamOrigin.x(), sharedWorldHeight, seamOrigin.z()}).y();
                    }
                    scene.editor().modify<Engine::TerrainComponent>(firstTarget.entity, [&](auto& terrain) {
                        for (std::uint32_t sample = 0; sample <= last; ++sample)
                            terrain.heights[edgeIndex(firstEdge, sample, terrain.resolution)] = firstSharedHeights[sample];
                        terrain.updateMeshRegion(*firstTarget.workingMesh, {0, 0, last, last, true});
                    });
                    scene.editor().modify<Engine::TerrainComponent>(secondTarget.entity, [&](auto& terrain) {
                        for (std::uint32_t sample = 0; sample <= last; ++sample) {
                            const std::uint32_t other = reverseOrder ? last - sample : sample;
                            terrain.heights[edgeIndex(secondEdge, other, terrain.resolution)] = secondSharedHeights[other];
                        }
                        terrain.updateMeshRegion(*secondTarget.workingMesh, {0, 0, last, last, true});
                    });
                    firstTarget.dirty = {0, 0, last, last, true};
                    secondTarget.dirty = {0, 0, last, last, true};
                }
            }
        }
    }
}

void finishTerrainStroke(Engine::ScenePreset& scene, TerrainSculptState& state) {
    if (!state.sculptTargets.empty()) {
        stitchSculptTerrainEdges(scene, state);
        for (const TerrainStrokeTarget& target : state.sculptTargets) {
            if (!scene.editor().valid(target.entity)) continue;
            if (target.workingMesh && scene.editor().has<Engine::ColliderComponent>(target.entity)) {
                scene.editor().modify<Engine::ColliderComponent>(target.entity, [&](auto& collider) {
                    if (auto* meshCollider = std::get_if<Engine::MeshCollider>(&collider.shape))
                        meshCollider->mesh = target.workingMesh;
                });
            }
            if (target.dirty.valid && scene.editor().has<Engine::TerrainComponent>(target.entity) &&
                scene.editor().has<Engine::TerrainGrassComponent>(target.entity)) {
                const auto& terrain = scene.editor().get<Engine::TerrainComponent>(target.entity);
                scene.editor().modify<Engine::TerrainGrassComponent>(target.entity, [&](auto& grass) {
                    for (auto& instance : grass.instances)
                        instance.position.setY(terrain.sampleHeight(instance.position.x(), instance.position.z()));
                    grass.allInstancesDirty = true;
                });
            }
        }
        state.strokeCompleted = state.strokeDirty.valid;
        state.sculptTargets.clear();
        state.strokeActive = false;
        state.strokeEntity = Engine::NullEntity;
        state.hasPreviousPoint = false;
        state.hasPreviousSculptWorldPoint = false;
        state.strokeDirty = {};
        return;
    }
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
    state.hasPreviousSculptWorldPoint = false;
    state.workingMesh.reset();
    state.strokeDirty = {};
}

bool drawTerrainSculpt(Engine::ScenePreset& scene, const Engine::Entity selected,
                       const Engine::Renderer& renderer,
                       const ImVec2 min, const ImVec2 max, TerrainSculptState& state,
                       const bool imageHovered, ViewportInteraction& interaction) {
    if (!state.enabled) return false;
    const bool selectedTerrain = selected != Engine::NullEntity && scene.editor().valid(selected) &&
        scene.editor().has<Engine::TerrainComponent>(selected) &&
        scene.editor().has<Engine::MeshRenderer>(selected) && scene.editor().has<Engine::Transform>(selected);
    if (!selectedTerrain && !state.strokeActive) return false;

    // The Scene View image is no longer ImGui's last item here: drawing the
    // viewport gizmo toolbar creates invisible buttons after it.  Keep using
    // the hover state captured directly after ImGui::Image instead.
    const bool hovered = imageHovered;
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const Engine::Camera camera = sceneViewCamera(renderer, min, max);
    const Engine::Vec3 rayDirection = viewportRayDirection(camera, mouse, min, max);

    Engine::Entity hitEntity{Engine::NullEntity};
    std::optional<Engine::Vec3> hit;
    if (hovered) {
        float nearestDistance = std::numeric_limits<float>::max();
        scene.editor().view<Engine::TerrainComponent, Engine::MeshRenderer, Engine::Transform>(
            [&](const Engine::Entity entity, const Engine::TerrainComponent& candidate,
                const Engine::MeshRenderer& meshRenderer, const Engine::Transform& candidateTransform) {
                if (!meshRenderer.hasMesh()) return;
                const auto candidateHit = raycastTerrain(candidate, candidateTransform, camera.position(), rayDirection);
                if (!candidateHit) return;
                const float distance = (*candidateHit - camera.position()).length();
                if (distance < nearestDistance) {
                    nearestDistance = distance;
                    hitEntity = entity;
                    hit = candidateHit;
                }
            });
    }
    const bool hitsTerrain = hit.has_value();
    Engine::Vec3 localHit{};
    if (hitsTerrain) {
        const auto& transform = scene.editor().get<Engine::Transform>(hitEntity);
        const auto& terrain = scene.editor().get<Engine::TerrainComponent>(hitEntity);
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

    if (hovered && hitsTerrain && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        state.strokeActive = true;
        state.strokeEntity = hitEntity;
        state.flattenHeight = localHit.y();
        state.hasPreviousPoint = false;
        state.hasPreviousSculptWorldPoint = false;
        state.heightsBeforeStroke = scene.editor().get<Engine::TerrainComponent>(hitEntity).heights;
        state.strokeDirty = {};
        scene.editor().view<Engine::TerrainComponent, Engine::MeshRenderer, Engine::Transform>(
            [&](const Engine::Entity entity, const Engine::TerrainComponent& targetTerrain,
                const Engine::MeshRenderer& meshRenderer, const Engine::Transform&) {
                if (!meshRenderer.hasMesh()) return;
                // A neighbour may still be displayed at a preview LOD.  A
                // sculpt stroke must always update its full-resolution mesh.
                auto workingMesh = std::make_shared<Engine::Mesh>(targetTerrain.createMesh());
                state.sculptTargets.push_back({entity, workingMesh, {}});
                scene.editor().modify<Engine::MeshRenderer>(entity, [&](auto& component) {
                    component.mesh = std::move(workingMesh);
                });
            });
        // Every terrain is registered; TerrainComponent::sculpt naturally
        // ignores targets outside the brush radius.
        state.workingMesh.reset();
    }
    if (state.strokeActive && ImGui::IsMouseDown(ImGuiMouseButton_Left) && hitsTerrain) {
        const ImGuiIO& io = ImGui::GetIO();
        if (io.KeyCtrl) state.flattenHeight = localHit.y();
        Engine::TerrainSculptMode strokeMode = state.mode;
        if (io.KeyShift && strokeMode == Engine::TerrainSculptMode::Raise) strokeMode = Engine::TerrainSculptMode::Lower;
        else if (io.KeyShift && strokeMode == Engine::TerrainSculptMode::Lower) strokeMode = Engine::TerrainSculptMode::Raise;
        const Engine::TerrainSculptMode configuredMode = state.mode;
        state.mode = strokeMode;
        const float deltaTime = std::clamp(static_cast<float>(Engine::Time::deltaTime()),
                                           1.0F / 240.0F, 1.0F / 20.0F);
        const float distance = state.hasPreviousSculptWorldPoint
            ? (*hit - state.previousSculptWorldPoint).length() : 0.0F;
        const float interval = std::max(state.radius * state.spacing, 0.01F);
        const int sampleCount = state.hasPreviousSculptWorldPoint
                                    ? std::max(1, static_cast<int>(std::ceil(distance / interval))) : 1;
        for (int sample = 1; sample <= sampleCount; ++sample) {
            const float t = static_cast<float>(sample) / static_cast<float>(sampleCount);
            const Engine::Vec3 worldPoint = state.hasPreviousSculptWorldPoint
                ? state.previousSculptWorldPoint + (*hit - state.previousSculptWorldPoint) * t : *hit;
            for (TerrainStrokeTarget& target : state.sculptTargets) {
                if (!scene.editor().valid(target.entity) || !target.workingMesh ||
                    !scene.editor().has<Engine::Transform>(target.entity)) continue;
                const auto& targetTransform = scene.editor().get<Engine::Transform>(target.entity);
                const Engine::Vec3 targetPoint = terrainLocalPoint(targetTransform, worldPoint);
                Engine::TerrainRegion dirty;
                if (!applyTerrainBrush(scene, target.entity, targetPoint, state.strength * deltaTime /
                                       static_cast<float>(sampleCount), state, *target.workingMesh, dirty)) continue;
                if (!target.dirty.valid) target.dirty = dirty;
                else {
                    target.dirty.minimumX = std::min(target.dirty.minimumX, dirty.minimumX);
                    target.dirty.minimumZ = std::min(target.dirty.minimumZ, dirty.minimumZ);
                    target.dirty.maximumX = std::max(target.dirty.maximumX, dirty.maximumX);
                    target.dirty.maximumZ = std::max(target.dirty.maximumZ, dirty.maximumZ);
                }
                state.strokeDirty.valid = true;
                interaction.terrainGeometryChanged = true;
                if (std::ranges::find(interaction.terrainGeometryEntities, target.entity) ==
                    interaction.terrainGeometryEntities.end())
                    interaction.terrainGeometryEntities.push_back(target.entity);
            }
        }
        // Smooth uses samples surrounding each affected vertex.  On a tile
        // boundary the two heightmaps therefore diverge immediately unless
        // the shared edge is synchronized during the stroke, not merely when
        // the mouse button is released.
        stitchSculptTerrainEdges(scene, state);
        for (const TerrainStrokeTarget& target : state.sculptTargets) {
            if (!target.workingMesh) continue;
            interaction.terrainGeometryChanged = true;
            if (std::ranges::find(interaction.terrainGeometryEntities, target.entity) ==
                interaction.terrainGeometryEntities.end())
                interaction.terrainGeometryEntities.push_back(target.entity);
        }
        state.mode = configuredMode;
        state.previousSculptWorldPoint = *hit;
        state.hasPreviousSculptWorldPoint = true;
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    }
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        finishTerrainStroke(scene, state);
        if (state.strokeCompleted)
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
            if (std::ranges::find(interaction.terrainGeometryEntities, selected) ==
                interaction.terrainGeometryEntities.end())
                interaction.terrainGeometryEntities.push_back(selected);
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

enum class SceneViewMode : std::uint8_t { Lit, Unlit, LightingOnly, Wireframe, Normals, Collision, Overdraw };

struct SceneViewSettings final {
    SceneViewMode mode{SceneViewMode::Lit};
    bool showCameraGizmos{true};
    bool showLightGizmos{true};
    bool showColliders{false};
    bool showMeshDiagnostics{true};
    struct Bookmark final { Engine::Vec3 position{}; float yaw{}; float pitch{}; };
    std::array<std::optional<Bookmark>, 4> bookmarks{};
};

const char* sceneViewModeName(const SceneViewMode mode) {
    switch (mode) {
        case SceneViewMode::Lit: return "Lit";
        case SceneViewMode::Unlit: return "Unlit";
        case SceneViewMode::LightingOnly: return "Lighting only";
        case SceneViewMode::Wireframe: return "Wireframe";
        case SceneViewMode::Normals: return "Normals";
        case SceneViewMode::Collision: return "Collision";
        case SceneViewMode::Overdraw: return "Overdraw";
    }
    return "Lit";
}

void drawColliderDiagnostics(const Engine::ScenePreset& scene, const Engine::Renderer& renderer,
                             const ImVec2 min, const ImVec2 max) {
    const Engine::Camera camera = sceneViewCamera(renderer, min, max);
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    scene.editor().view<Engine::ColliderComponent, Engine::Transform>(
        [&](const Engine::Entity, const Engine::ColliderComponent& collider,
            const Engine::Transform& transform) {
            Engine::Vec3 half{0.5F, 0.5F, 0.5F};
            std::visit([&](const auto& shape) {
                using Shape = std::decay_t<decltype(shape)>;
                if constexpr (std::is_same_v<Shape, Engine::BoxCollider> ||
                              std::is_same_v<Shape, Engine::RampCollider>) half = shape.halfExtents;
                else if constexpr (std::is_same_v<Shape, Engine::SphereCollider>)
                    half = {shape.radius, shape.radius, shape.radius};
                else if constexpr (std::is_same_v<Shape, Engine::CapsuleCollider>)
                    half = {shape.radius, shape.height * 0.5F, shape.radius};
            }, collider.shape);
            const glm::mat4 matrix = transform.matrix().native();
            const Engine::Vec3 center{matrix * glm::vec4{collider.offset.native(), 1.0F}};
            const Engine::Vec3 corners[8]{
                {-half.x(), -half.y(), -half.z()}, {half.x(), -half.y(), -half.z()},
                {half.x(), half.y(), -half.z()}, {-half.x(), half.y(), -half.z()},
                {-half.x(), -half.y(), half.z()}, {half.x(), -half.y(), half.z()},
                {half.x(), half.y(), half.z()}, {-half.x(), half.y(), half.z()}};
            ImVec2 points[8];
            for (int index = 0; index < 8; ++index) {
                points[index] = projectGizmoPoint(camera,
                    Engine::Vec3{matrix * glm::vec4{(corners[index] + collider.offset).native(), 1.0F}}, min, max);
            }
            constexpr int edges[12][2]{{0,1},{1,2},{2,3},{3,0},{4,5},{5,6},{6,7},{7,4},{0,4},{1,5},{2,6},{3,7}};
            const ImU32 color = collider.isTrigger ? IM_COL32(255, 180, 55, 230) : IM_COL32(65, 235, 115, 230);
            for (const auto& edge : edges) drawList->AddLine(points[edge[0]], points[edge[1]], color, 1.5F);
            const ImVec2 projectedCenter = projectGizmoPoint(camera, center, min, max);
            drawList->AddCircleFilled(projectedCenter, 3.0F, color);
        });
}

void focusSceneView(const Engine::ScenePreset& scene, const Engine::Entity entity,
                    Engine::Renderer& renderer, const ImVec2 min, const ImVec2 max) {
    if (entity == Engine::NullEntity || !scene.editor().valid(entity) ||
        !scene.editor().has<Engine::Transform>(entity)) return;
    const Engine::Vec3 target = renderer.editorGizmoPosition(entity);
    const Engine::Camera camera = sceneViewCamera(renderer, min, max);
    renderer.setEditorCameraPosition(target - camera.forward() * 6.0F);
}

void frameAllSceneView(const Engine::ScenePreset& scene, Engine::Renderer& renderer,
                       const ImVec2 min, const ImVec2 max) {
    Engine::Vec3 center{};
    std::size_t count{};
    scene.editor().view<Engine::Transform>([&](const Engine::Entity, const Engine::Transform& transform) {
        center += transform.position; ++count;
    });
    if (count == 0) return;
    center = center * (1.0F / static_cast<float>(count));
    float radius = 2.0F;
    scene.editor().view<Engine::Transform>([&](const Engine::Entity, const Engine::Transform& transform) {
        radius = std::max(radius, (transform.position - center).length());
    });
    const Engine::Camera camera = sceneViewCamera(renderer, min, max);
    renderer.setEditorCameraPosition(center - camera.forward() * (radius * 2.2F + 2.0F));
}

ViewportInteraction drawViewport(Engine::ScenePreset &scene, Engine::Assets::Content& content,
                                 const Engine::Entity selected,
                                 const std::vector<Engine::Entity>& selection,
                                 Engine::Renderer &renderer,
                                 Engine::ViewportHandle gameDescriptor,
                                 Engine::ViewportHandle sceneDescriptor,
                                 const float sceneCameraYaw, const float sceneCameraPitch,
                                 bool &showGameView, GizmoMode &gizmoMode,
                                 SelectionTool& selectionTool, TerrainSculptState& terrainSculpt,
                                 const bool playing, bool& isOpen) {
    static std::string assetDropError;
    static SceneViewSettings settings;
    if (playing) {
        showGameView = true;
    }
    const Engine::ViewportHandle descriptor = showGameView ? gameDescriptor : sceneDescriptor;

    ImGui::Begin("Viewport", &isOpen, ImGuiWindowFlags_NoScrollbar);
    ViewportInteraction interaction{};
    if (!playing) {
        ImGui::PushStyleColor(ImGuiCol_Button, {0.12F, 0.40F, 0.28F, 1.0F});
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, {0.20F, 0.56F, 0.38F, 1.0F});
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, {0.10F, 0.30F, 0.22F, 1.0F});
        if (EditorButton(" Play Mode ").draw()) interaction.playModeAction = PlayModeAction::Start;
        ImGui::PopStyleColor(3);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Start Play Mode (F5)");
    } else {
        ImGui::PushStyleColor(ImGuiCol_Button, {0.58F, 0.20F, 0.22F, 1.0F});
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, {0.74F, 0.28F, 0.30F, 1.0F});
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, {0.45F, 0.14F, 0.16F, 1.0F});
        if (EditorButton(" Stop ").draw()) interaction.playModeAction = PlayModeAction::Stop;
        ImGui::PopStyleColor(3);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Stop Play Mode and restore the editor scene (F5)");
        ImGui::SameLine(0.0F, 6.0F);
        ImGui::PushStyleColor(ImGuiCol_Button, {0.16F, 0.34F, 0.56F, 1.0F});
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, {0.24F, 0.46F, 0.72F, 1.0F});
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, {0.11F, 0.25F, 0.42F, 1.0F});
        if (EditorButton(" Restart ").draw()) interaction.playModeAction = PlayModeAction::Restart;
        ImGui::PopStyleColor(3);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Restart Play Mode from the editor scene");
    }
    ImGui::SameLine(0.0F, 10.0F);
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
        if (!showGameView) {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(130.0F);
            if (ImGui::BeginCombo("##scene-view-mode", sceneViewModeName(settings.mode))) {
                for (int mode = 0; mode <= static_cast<int>(SceneViewMode::Overdraw); ++mode) {
                    const auto value = static_cast<SceneViewMode>(mode);
                    if (ImGui::Selectable(sceneViewModeName(value), settings.mode == value)) settings.mode = value;
                }
                ImGui::EndCombo();
            }
            ImGui::SameLine();
            if (ImGui::Button("Visibility##scene")) ImGui::OpenPopup("Scene visibility");
            if (ImGui::BeginPopup("Scene visibility")) {
                ImGui::TextDisabled("Gizmo categories");
                ImGui::Checkbox("Cameras", &settings.showCameraGizmos);
                ImGui::Checkbox("Lights", &settings.showLightGizmos);
                ImGui::Checkbox("Colliders", &settings.showColliders);
                ImGui::Checkbox("Mesh diagnostics", &settings.showMeshDiagnostics);
                ImGui::Separator();
                ImGui::TextDisabled("Rendering categories remain visible; use a diagnostic mode to inspect them.");
                ImGui::EndPopup();
            }
        }
        ImGui::Spacing();
    }
    bool viewportHovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
    const ImVec2 size = ImGui::GetContentRegionAvail();
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
        if (playing && showGameView && imageHovered &&
            ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            interaction.gameMouseCaptureRequested = true;
        }
        if (imageHovered && !assetDropError.empty()) {
            ImGui::SetTooltip("Could not add model: %s", assetDropError.c_str());
        }
        viewportHovered = imageHovered;
        int gizmoAction = -1;
        bool gizmoToolsConsumeClick = false;
        bool orientationGizmoConsumesClick = false;
        if (!showGameView && !playing) {
            gizmoToolsConsumeClick = drawViewportGizmoTools(imageMin, ImGui::GetWindowPos(), gizmoMode);
            drawSelectionTools(imageMin, ImGui::GetWindowPos(), selectionTool);
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
            if (settings.showCameraGizmos) drawCameraGizmos(scene, selected, renderer, imageMin, imageMax);
            if (settings.showLightGizmos) drawLightGizmos(scene, selected, renderer, imageMin, imageMax);
            if (settings.showColliders || settings.mode == SceneViewMode::Collision)
                drawColliderDiagnostics(scene, renderer, imageMin, imageMax);

            // These modes are deliberately overlays: the renderer stays on its normal material
            // path, preserving picking and MSAA while exposing the diagnostic intent in-place.
            ImDrawList* drawList = ImGui::GetWindowDrawList();
            if (settings.mode == SceneViewMode::Unlit)
                drawList->AddRectFilled(imageMin, imageMax, IM_COL32(220, 220, 220, 55));
            else if (settings.mode == SceneViewMode::LightingOnly)
                drawList->AddRectFilled(imageMin, imageMax, IM_COL32(20, 35, 65, 125));
            else if (settings.mode == SceneViewMode::Wireframe) {
                const Engine::Camera camera = sceneViewCamera(renderer, imageMin, imageMax);
                scene.editor().view<Engine::MeshRenderer, Engine::Transform>(
                    [&](const Engine::Entity, const Engine::MeshRenderer& meshRenderer, const Engine::Transform& transform) {
                        if (!meshRenderer.hasMesh() || !settings.showMeshDiagnostics) return;
                        const auto& mesh = *meshRenderer.mesh;
                        const std::size_t limit = std::min<std::size_t>(mesh.indices.size(), 900);
                        for (std::size_t index = 0; index + 2 < limit; index += 3) {
                            const auto project = [&](const std::uint32_t vertex) {
                                return projectGizmoPoint(camera, Engine::Vec3{transform.matrix().native() *
                                    glm::vec4{mesh.vertices[vertex].position.native(), 1.0F}}, imageMin, imageMax);
                            };
                            const ImVec2 a = project(mesh.indices[index]), b = project(mesh.indices[index + 1]), c = project(mesh.indices[index + 2]);
                            drawList->AddTriangle(a, b, c, IM_COL32(75, 235, 255, 180), 1.0F);
                        }
                    });
            } else if (settings.mode == SceneViewMode::Normals) {
                const Engine::Camera camera = sceneViewCamera(renderer, imageMin, imageMax);
                scene.editor().view<Engine::MeshRenderer, Engine::Transform>(
                    [&](const Engine::Entity, const Engine::MeshRenderer& meshRenderer, const Engine::Transform& transform) {
                        if (!meshRenderer.hasMesh() || !settings.showMeshDiagnostics) return;
                        const auto& vertices = meshRenderer.mesh->vertices;
                        const std::size_t stride = std::max<std::size_t>(1, vertices.size() / 80);
                        for (std::size_t index = 0; index < vertices.size(); index += stride) {
                            const auto& vertex = vertices[index];
                            const Engine::Vec3 start{transform.matrix().native() * glm::vec4{vertex.position.native(), 1.0F}};
                            const Engine::Vec3 end{transform.matrix().native() * glm::vec4{vertex.position.native() + vertex.normal.native() * 0.25F, 1.0F}};
                            drawProjectedCameraLine(drawList, camera, start, end, imageMin, imageMax, IM_COL32(80, 220, 255, 220), 1.25F);
                        }
                    });
            } else if (settings.mode == SceneViewMode::Overdraw) {
                scene.editor().view<Engine::Transform>([&](const Engine::Entity, const Engine::Transform& transform) {
                    const ImVec2 point = projectGizmoPoint(sceneViewCamera(renderer, imageMin, imageMax), transform.position, imageMin, imageMax);
                    drawList->AddCircleFilled(point, 45.0F, IM_COL32(255, 55, 20, 18));
                });
            }

            ImGui::SetCursorScreenPos({imageMin.x + 10.0F, imageMax.y - 30.0F});
            if (ImGui::Button("Focus selected (F)")) focusSceneView(scene, selected, renderer, imageMin, imageMax);
            ImGui::SameLine();
            if (ImGui::Button("Frame all")) frameAllSceneView(scene, renderer, imageMin, imageMax);
            ImGui::SameLine();
            if (ImGui::Button("Bookmarks")) ImGui::OpenPopup("Scene camera bookmarks");
            if (ImGui::BeginPopup("Scene camera bookmarks")) {
                for (std::size_t slot = 0; slot < settings.bookmarks.size(); ++slot) {
                    const std::string label = "Slot " + std::to_string(slot + 1);
                    if (ImGui::MenuItem(("Save " + label).c_str())) settings.bookmarks[slot] =
                        SceneViewSettings::Bookmark{renderer.editorCameraPosition(), renderer.editorCameraYaw(), renderer.editorCameraPitch()};
                    if (settings.bookmarks[slot] && ImGui::MenuItem(("Recall " + label).c_str())) {
                        renderer.setEditorCameraPosition(settings.bookmarks[slot]->position);
                        renderer.setEditorCameraRotation(settings.bookmarks[slot]->yaw, settings.bookmarks[slot]->pitch);
                    }
                }
                ImGui::EndPopup();
            }
            if (selected != Engine::NullEntity && scene.editor().valid(selected) &&
                scene.editor().has<Engine::CameraComponent>(selected) &&
                scene.editor().has<Engine::Transform>(selected)) {
                ImGui::SameLine();
                if (ImGui::Button("Align active camera to view")) {
                    scene.editor().modify<Engine::Transform>(selected, [&](Engine::Transform& transform) {
                        transform.position = renderer.editorCameraPosition();
                        transform.rotation.setX(renderer.editorCameraPitch());
                        transform.rotation.setY(renderer.editorCameraYaw());
                    });
                }
            }
            if (imageHovered && !ImGui::GetIO().WantTextInput && ImGui::IsKeyPressed(ImGuiKey_F)) {
                focusSceneView(scene, selected, renderer, imageMin, imageMax);
            }
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
                 ? drawTranslationGizmo(scene, selected, selection, renderer, imageMin, imageMax)
                 : gizmoMode == GizmoMode::Rotate
                       ? drawRotationGizmo(scene, selected, selection, renderer, imageMin, imageMax)
                       : drawScaleGizmo(scene, selected, selection, renderer, imageMin, imageMax));
        const bool gizmoConsumesClick = gizmoToolsConsumeClick || orientationGizmoConsumesClick ||
            sculptConsumesClick || paintConsumesClick || grassConsumesClick ||
            transformGizmoConsumesClick;
        static bool selectionDrag = false;
        static ImVec2 selectionStart{};
        static std::vector<ImVec2> lasso;
        const ImVec2 mouse = ImGui::GetIO().MousePos;
        if (!showGameView && !playing && gizmoAction < 0 && !gizmoConsumesClick && imageHovered &&
            ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            selectionDrag = true;
            selectionStart = mouse;
            lasso = {mouse};
        }
        if (selectionDrag && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            if (selectionTool == SelectionTool::Lasso &&
                (lasso.empty() || std::hypot(mouse.x - lasso.back().x, mouse.y - lasso.back().y) > 4.0F)) lasso.push_back(mouse);
            ImDrawList* drawList = ImGui::GetWindowDrawList();
            if (selectionTool == SelectionTool::Rectangle) {
                drawList->AddRect(selectionStart, mouse, IM_COL32(70, 205, 235, 255), 0.0F, 0, 1.5F);
            } else if (lasso.size() > 1) drawList->AddPolyline(lasso.data(), static_cast<int>(lasso.size()), IM_COL32(70, 205, 235, 255), ImDrawFlags_None, 1.5F);
        }
        if (selectionDrag && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
            selectionDrag = false;
            const bool dragMeaningful = std::hypot(mouse.x - selectionStart.x, mouse.y - selectionStart.y) > 6.0F;
            if (dragMeaningful) {
                const Engine::Camera camera = sceneViewCamera(renderer, imageMin, imageMax);
                const float left = std::min(selectionStart.x, mouse.x), right = std::max(selectionStart.x, mouse.x);
                const float top = std::min(selectionStart.y, mouse.y), bottom = std::max(selectionStart.y, mouse.y);
                scene.editor().view<Engine::Transform>([&](const Engine::Entity entity, const Engine::Transform& transform) {
                    // Terrain is a surface used to frame the scene, not an object
                    // meant for freeform lasso selection. Keep rectangle selection
                    // unchanged so terrain can still be selected intentionally.
                    if (selectionTool == SelectionTool::Lasso &&
                        scene.editor().has<Engine::TerrainComponent>(entity)) return;
                    const ImVec2 point = projectGizmoPoint(camera, transform.position, imageMin, imageMax);
                    const bool inRectangle = point.x >= left && point.x <= right && point.y >= top && point.y <= bottom;
                    if ((selectionTool == SelectionTool::Rectangle ? inRectangle : lasso.size() > 2 && pointInLasso(point, lasso)))
                        interaction.selectedEntities.push_back(entity);
                });
                interaction.selectionCommitted = true;
            } else {
            interaction.sceneClicked = true;
            interaction.normalizedX = ((mouse.x - imageMin.x) / (imageMax.x - imageMin.x)) * 2.0F - 1.0F;
            interaction.normalizedY = ((mouse.y - imageMin.y) / (imageMax.y - imageMin.y)) * 2.0F - 1.0F;
            }
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
    // Play Mode owns the cursor for the whole session. Requiring the Game
    // View to be hovered let ImGui release relative mouse mode as soon as the
    // pointer left its image, which breaks mouse-look at the window edge.
    interaction.gameCameraInput = playing && showGameView;
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
