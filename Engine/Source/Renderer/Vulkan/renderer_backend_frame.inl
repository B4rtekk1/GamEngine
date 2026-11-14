static std::array<glm::vec4, 6> extractFrustumPlanes(const glm::mat4& matrix) {
    const glm::vec4 row0{matrix[0][0], matrix[1][0], matrix[2][0], matrix[3][0]};
    const glm::vec4 row1{matrix[0][1], matrix[1][1], matrix[2][1], matrix[3][1]};
    const glm::vec4 row2{matrix[0][2], matrix[1][2], matrix[2][2], matrix[3][2]};
    const glm::vec4 row3{matrix[0][3], matrix[1][3], matrix[2][3], matrix[3][3]};
    std::array<glm::vec4, 6> planes{row3 + row0, row3 - row0, row3 + row1, row3 - row1, row2, row3 - row2};

    for (glm::vec4& plane : planes) {
        const float length = glm::length(glm::vec3{plane});
        if (length > 1.0e-6F)
            plane /= length;
    }
    return planes;
}

// Per-meshlet VSM caster stream for ShadowPass::record; inert (and the
// one-sided stream drawn whole) when this scene has no meshlet heap.
// Indexed meshlet draws read the shared index heap (GpuMeshlet::material.y
// holds each meshlet's first index there); only mesh-shader capable devices
// build meshlet streams.
[[nodiscard]] bool meshletIndexedDrawsSupported() const noexcept {
    return vulkanDevice.supportsMeshShaders() && indexBuffer.handle() != VK_NULL_HANDLE;
}

[[nodiscard]] ShadowPass::MeshletStream shadowMeshletStream(const std::uint32_t frame,
                                                           const bool twoSided = false) const {
    const VkDescriptorSet drawSet = twoSided ? shadowTwoSidedMeshletDrawSets[frame] : shadowMeshletDrawSets[frame];
    const VkDescriptorSet refineSet =
        twoSided ? shadowTwoSidedMeshletRefineSets[frame] : shadowMeshletRefineSets[frame];
    // Mesh-shader casters need the meshlet vertex streams the draw set binds.
    const bool meshCasters = optimizationFeatures.meshShaders && drawSet != VK_NULL_HANDLE &&
                             meshletVertexBuffer.handle() != VK_NULL_HANDLE &&
                             meshletTriangleBuffer.handle() != VK_NULL_HANDLE;
    const auto& objectCounts = twoSided ? shadowTwoSidedMeshletObjectCountBuffers : shadowMeshletObjectCountBuffers;
    const auto& commands = twoSided ? shadowTwoSidedMeshletCommandBuffers : shadowMeshletCommandBuffers;
    const auto& commandCounts =
        twoSided ? shadowTwoSidedMeshletCommandCountBuffers : shadowMeshletCommandCountBuffers;
    const auto& taskCommands = twoSided ? shadowTwoSidedMeshletTaskCommandBuffers : shadowMeshletTaskCommandBuffers;
    return {.refinePipeline = shadowMeshletRefinePipeline,
            .refineLayout = shadowMeshletRefinePipelineLayout,
            .refineSet = meshletBuffer.handle() != VK_NULL_HANDLE ? refineSet : VK_NULL_HANDLE,
            .objectCounts = objectCounts[frame].handle(),
            .commands = commands[frame].handle(),
            .commandCounts = commandCounts[frame].handle(),
            .meshletIndexBuffer = meshletIndexedDrawsSupported() ? indexBuffer.handle() : VK_NULL_HANDLE,
            .passCapacity = twoSided ? shadowTwoSidedMeshletPassCapacity : shadowMeshletPassCapacity,
            .meshletCount = globalMeshletCount,
            .meshRefinePipeline = meshCasters ? shadowMeshletMeshRefinePipeline : VK_NULL_HANDLE,
            .meshCommandPipeline = meshCasters ? shadowMeshletMeshCommandPipeline : VK_NULL_HANDLE,
            .meshTaskCommands = meshCasters ? taskCommands[frame].handle() : VK_NULL_HANDLE,
            .meshDrawSet = meshCasters ? drawSet : VK_NULL_HANDLE};
}

[[nodiscard]] DirectionalLight directionalLight() const noexcept {
    return sceneFrameDataCache.data.directionalLight;
}

// The photometric profile of a point or spot light, or nothing for an
// unprofiled one. IES files resolve against the project's asset folder like
// the scene sky, and are parsed once per path.
[[nodiscard]] std::optional<LightProfileTable> localLightProfileTable(const LightComponent& light) {
    if (light.profile == LightProfile::None)
        return std::nullopt;
    if (light.profile != LightProfile::Ies)
        return builtInLightProfile(light.profile);
    if (light.iesProfile.empty())
        return std::nullopt;
    std::filesystem::path path = light.iesProfile;
    if (path.is_relative()) {
        const auto runtimeRoot = projectRoot.empty() ? assetManager.asset_root() : projectRoot;
        const auto content = runtimeRoot / "Content";
        path = (std::filesystem::is_directory(content) ? content : runtimeRoot / "Assets") / path;
    }
    const auto key = path.lexically_normal().generic_string();
    if (const auto cached = iesProfileCache.find(key); cached != iesProfileCache.end())
        return cached->second;
    auto table = loadIesProfile(path);
    if (!table)
        Diagnostics::instance().report(DiagnosticSeverity::Warning,
                                       "Could not read IES light profile: " + path.string() +
                                           "; the light shines without a profile.",
                                       {.subsystem = "Renderer"});
    return iesProfileCache.emplace(key, std::move(table)).first->second;
}

void refreshSceneFrameData() {
    const Registry& readRegistry = registry;
    const std::uint64_t transformRevision = readRegistry.componentRevision<Transform>();
    const std::uint64_t lightRevision = readRegistry.componentRevision<LightComponent>();
    const std::uint64_t windRevision = readRegistry.componentRevision<WindComponent>();
    const std::uint64_t cameraRevision = readRegistry.componentRevision<CameraComponent>();
    const std::uint64_t uuidRevision = readRegistry.componentRevision<UUIDComponent>();
    const std::uint64_t structuralRevision = readRegistry.structuralRevision();
    const bool uuidIndexDirty = !sceneFrameDataCache.initialized || sceneFrameDataCache.uuidRevision != uuidRevision ||
                                sceneFrameDataCache.structuralRevision != structuralRevision;
    if (uuidIndexDirty) {
        auto& entitiesByUuid = sceneFrameDataCache.entitiesByUuid;
        entitiesByUuid.clear();
        entitiesByUuid.reserve(readRegistry.size());
        readRegistry.view<UUIDComponent>(
            [&](const Entity entity, const UUIDComponent& uuid) { entitiesByUuid.emplace(uuid.value, entity); });
    }

    // TransformSystem already records the world transforms it actually
    // recomputed, including descendants of a moved parent. A moving
    // renderable must not force scans of every camera, light and wind
    // component just because Transform has a global revision.
    // Read every world change since the last refresh, not just the latest
    // update's: physics resolves transforms before the renderer does, and a
    // camera or light change it resolved first was otherwise lost, so the
    // view lagged and jumped (and temporal effects saw that motion).
    bool relevantTransformChanged = !sceneFrameDataCache.initialized;
    if (!relevantTransformChanged && sceneFrameDataCache.transformRevision != transformRevision) {
        bool complete = true;
        const auto changes =
            TransformSystem::worldTransformChangesSince(registry, sceneFrameDataCache.worldTransformSerial, complete);
        relevantTransformChanged = !complete;
        for (const auto& change : changes) {
            if (relevantTransformChanged)
                break;
            const Entity entity = change.entity;
            if (readRegistry.has<CameraComponent>(entity) || readRegistry.has<LightComponent>(entity) ||
                readRegistry.has<WindComponent>(entity))
                relevantTransformChanged = true;
        }
    }
    sceneFrameDataCache.worldTransformSerial = TransformSystem::worldTransformSerial(registry);

    const bool dataDirty =
        !sceneFrameDataCache.initialized || relevantTransformChanged ||
        sceneFrameDataCache.lightRevision != lightRevision || sceneFrameDataCache.windRevision != windRevision ||
        sceneFrameDataCache.cameraRevision != cameraRevision || sceneFrameDataCache.uuidRevision != uuidRevision;
    if (dataDirty) {
        SceneFrameData data{};
        bool directionalLightFound = false;
        bool mainLightPresent = false;
        readRegistry.view<CameraComponent, Transform>(
            [&](const Entity entity, const CameraComponent& component, const Transform&) {
                if (data.primaryCamera == NullEntity && component.primary && component.isPerspective() &&
                    component.isValid()) {
                    data.primaryCamera = entity;
                    sceneFrameDataCache.primaryCameraComponent = component;
                }
            });
        readRegistry.view<Transform, LightComponent>(
            [&](const Entity, const Transform& transform, const LightComponent& light) {
                if (light.type == LightType::Directional && light.mainLight)
                    mainLightPresent = true;
                if (!light.enabled)
                    return;
                const glm::mat4 world = transform.worldMatrix().native();
                const glm::vec3 direction = glm::vec3(world * glm::vec4{0.0F, 0.0F, -1.0F, 0.0F});
                const Math::Color color = light.color;
                if (light.type == LightType::Directional) {
                    // Only the designated, enabled Main Light reaches the current forward
                    // path. Other directional lights stay enabled in ECS for future paths.
                    if (!directionalLightFound && light.mainLight && glm::length(direction) > 1e-6F) {
                        data.directionalLight.direction = Vec3{glm::normalize(direction)};
                        data.directionalLight.color = color;
                        data.directionalLight.intensity = std::max(0.0F, light.intensity);
                        data.directionalLight.enabled = true;
                        data.directionalLight.castShadows = light.castShadows;
                        directionalLightFound = true;
                    }
                    return;
                }
                if (data.lightCount == MaxClusteredLocalLights || light.range <= 0.0F)
                    return;
                constexpr float pi = 3.14159265358979323846F;
                auto& gpu = data.lights[data.lightCount++];
                const glm::vec3 position = glm::vec3{world[3]};
                gpu.positionRange = {position.x, position.y, position.z, light.range};
                gpu.directionOuterCos = {glm::normalize(direction), std::cos(light.outerConeAngle * pi / 180.0F)};
                gpu.colorIntensity = {color.r(), color.g(), color.b(), std::max(0.0F, light.intensity)};
                gpu.parameters = {std::cos(light.innerConeAngle * pi / 180.0F), static_cast<float>(light.type),
                                  light.castShadows ? 1.0F : 0.0F, 0.0F};
                gpu.profile = {};
                if (const auto table = localLightProfileTable(light)) {
                    gpu.profile = packLightProfile(*table);
                    gpu.parameters.w = 1.0F;
                }
            });
        if (const auto& sun = imageBasedLighting.sun(); sun) {
            if (data.directionalLight.enabled || !mainLightPresent) {
                // The panorama direction points toward the sun; the renderer's
                // directional light stores the direction in which rays travel.
                data.directionalLight.direction = -sun->direction;
                data.directionalLight.color = Math::Color{sun->color.x(), sun->color.y(), sun->color.z()};
                data.directionalLight.intensity = sun->intensity;
                if (!mainLightPresent) {
                    data.directionalLight.enabled = true;
                    data.directionalLight.castShadows = true;
                }
            }
        }
        sceneFrameDataCache.hasWind = false;
        readRegistry.view<Transform, WindComponent>(
            [&](const Entity, const Transform& transform, const WindComponent& wind) {
                if (sceneFrameDataCache.hasWind || !wind.enabled || wind.strength <= 0.0F || wind.range <= 0.0F)
                    return;
                const float length = wind.direction.length();
                if (length <= 1.0e-4F)
                    return;
                const Vec3 direction = wind.direction * (1.0F / length);
                data.wind.directionStrength = {direction.x(), direction.y(), direction.z(), wind.strength};
                const glm::vec3 position = glm::vec3{transform.worldMatrix().native()[3]};
                data.wind.sourcePositionRange = {position.x, position.y, position.z, wind.range};
                data.wind.gustFrequencyTime = {wind.gustStrength, wind.frequency, 0.0F, 0.0F};
                sceneFrameDataCache.hasWind = true;
            });
        if (data.primaryCamera != NullEntity) {
            const auto pose = sceneCameraPose(readRegistry, data.primaryCamera);
            sceneFrameDataCache.primaryCameraPosition = pose.position;
            sceneFrameDataCache.primaryCameraYaw = pose.yaw;
            sceneFrameDataCache.primaryCameraPitch = pose.pitch;
        }
        sceneFrameDataCache.data = std::move(data);
        sceneFrameDataCache.lightRevision = lightRevision;
        sceneFrameDataCache.windRevision = windRevision;
        sceneFrameDataCache.cameraRevision = cameraRevision;
    }
    // Time is deliberately refreshed every frame, while ECS-derived wind values stay cached.
    // Shader Graph's Time node currently reads the w component, so this
    // must not depend on a scene having a WindComponent.
    const float now = static_cast<float>(Time::elapsedTime());
    const Vec4& cachedWind = sceneFrameDataCache.data.wind.gustFrequencyTime;
    sceneFrameDataCache.data.wind.gustFrequencyTime = {cachedWind.x(), cachedWind.y(), now,
                                                       now - static_cast<float>(Time::deltaTime())};
    sceneFrameDataCache.uuidRevision = uuidRevision;
    // Advance this watermark even when only an unrelated Transform
    // changed, otherwise that same change would be inspected again on
    // every subsequent frame.
    sceneFrameDataCache.transformRevision = transformRevision;
    sceneFrameDataCache.structuralRevision = structuralRevision;
    sceneFrameDataCache.initialized = true;
}

void updateUniformBuffer(const uint32_t frame) {
    const auto componentRevision = registry.componentRevision<ReflectionProbeComponent>();
    const auto transformRevision = registry.componentRevision<Transform>();
    bool probesChanged = !reflectionProbeTableInitialized || componentRevision != reflectionProbeComponentRevision;
    if (!probesChanged && transformRevision != reflectionProbeTransformRevision) {
        registry.forEachComponentChangedSince<Transform>(reflectionProbeTransformRevision, [&](const Entity entity) {
            probesChanged = probesChanged || registry.has<ReflectionProbeComponent>(entity);
        });
    }
    reflectionProbeComponentRevision = componentRevision;
    reflectionProbeTransformRevision = transformRevision;
    if (probesChanged) {
        reflectionProbeManager.update(registry);
        reflectionProbeTableInitialized = true;
        reflectionProbeBufferDirtyMask = (1U << MAX_FRAMES_IN_FLIGHT) - 1U;
    }
    const std::uint32_t frameBit = 1U << frame;
    if ((reflectionProbeBufferDirtyMask & frameBit) != 0U) {
        const auto& reflectionProbes = reflectionProbeManager.probes();
        const GpuReflectionProbe emptyProbe{};
        reflectionProbeBuffers[frame].update(reflectionProbes.empty() ? &emptyProbe : reflectionProbes.data(),
                                             sizeof(GpuReflectionProbe) *
                                                 std::max<std::size_t>(1, reflectionProbes.size()));
        reflectionProbeManager.uploadProbeTable(frame);
        reflectionProbeBufferDirtyMask &= ~frameBit;
    }
    const auto& reflectionProbes = reflectionProbeManager.probes();
    const bool renderGameViewport = !editorUiActive || !sceneViewportActive;
    const SceneFrameData& frameData = sceneFrameDataCache.data;
    const bool mainLightShadows = frameData.directionalLight.enabled && frameData.directionalLight.castShadows &&
                                  optimizationFeatures.shadows && hasShadowCasters;
    const Entity activeCamera = frameData.primaryCamera;
    if (activeCamera == NullEntity) {
        // A malformed or incomplete scene must not stop rendering. Use
        // the editor camera as a predictable, controllable fallback
        // until the author adds or repairs a primary perspective camera.
        if (!fallbackCameraWarningReported) {
            Diagnostics::instance().report(
                DiagnosticSeverity::Warning, "No usable primary camera; rendering with the fallback camera.",
                {.subsystem = "Renderer",
                 .component = "CameraComponent",
                 .suggestedAction = "Add a perspective camera with Transform and mark it Primary."});
            fallbackCameraWarningReported = true;
        }
        cameraController.camera().emplace(Degrees{SCENE_CAMERA_FOV_DEGREES}, 16.0F / 9.0F, SCENE_CAMERA_NEAR_CLIP,
                                          SCENE_CAMERA_FAR_CLIP);
        cameraController.camera()->setPosition(cameraController.editorPosition());
        cameraController.camera()->setRotation(Degrees{cameraController.editorYaw()},
                                               Degrees{cameraController.editorPitch()});
    } else {
        fallbackCameraWarningReported = false;
        const auto& component = sceneFrameDataCache.primaryCameraComponent;

        // Game View is presented in a fixed 16:9 editor frame. Keep the
        // projection in that aspect too, independently of dock layout.
        const float gameAspect = editorUiActive ? (16.0F / 9.0F) : component.aspectRatio;
        cameraController.camera().emplace(Degrees{component.fieldOfView}, gameAspect, component.nearClip,
                                          component.farClip);
        // A camera's pitch is a view-space angle, not merely an X-axis
        // model rotation. Deriving it from the model matrix made pitch
        // depend on yaw, producing an unnatural vertical mouse look.
        cameraController.camera()->setPosition(sceneFrameDataCache.primaryCameraPosition);
        cameraController.camera()->setRotation(Degrees{sceneFrameDataCache.primaryCameraYaw},
                                               Degrees{sceneFrameDataCache.primaryCameraPitch});
    }

    taaJitterX = 0.0F;
    taaJitterY = 0.0F;
    cameraController.camera()->setProjectionJitter(0.0F, 0.0F);
    // The embedded Game View is also rendered before the ImGui pass.
    // Apply TAA there, but leave the independent Scene View camera
    // stable so editor gizmos and picking stay pixel-precise.
    const bool temporalAaWasActive = taaResolveActive;
    taaResolveActive = antialiasingLevel == AntialiasingLevel::TAA && (!editorUiActive || !sceneViewportActive);
    if (taaResolveActive && !temporalAaWasActive) {
        temporalAaPass.reset();
        taaSampleIndex = 0;
    }
    if (taaResolveActive) {
        constexpr std::uint32_t TaaSampleCount = 16;
        const auto halton = [](std::uint64_t index, const std::uint32_t base) {
            float result = 0.0F;
            float factor = 1.0F;
            while (index > 0) {
                factor /= static_cast<float>(base);
                result += factor * static_cast<float>(index % base);
                index /= base;
            }
            return result;
        };
        const VkExtent2D extent = swapchain.extent();
        const std::uint32_t sampleIndex = static_cast<std::uint32_t>(taaSampleIndex % TaaSampleCount);
        taaJitterX = (halton(sampleIndex + 1, 2) - 0.5F) * 2.0F / static_cast<float>(extent.width);
        taaJitterY = (halton(sampleIndex + 1, 3) - 0.5F) * 2.0F / static_cast<float>(extent.height);
        ++taaSampleIndex;
        cameraController.camera()->setProjectionJitter(taaJitterX, taaJitterY);
    }

    // Use a short burst after a camera cut (and during the first
    // frame) so the bounded VSM scheduler fills useful pages quickly.
    // Steady frames keep the much cheaper normal budget.
    const Vec3 currentCameraPosition = cameraController.camera()->position();
    const Vec3 currentCameraForward = cameraController.camera()->forward();
    const Mat4 currentView = cameraController.camera()->viewMatrix();
    const Mat4 currentProjection = cameraController.camera()->projectionMatrix();
    constexpr float cameraCutDistance = 5.0F;
    constexpr float cameraCutDirectionDot = 0.8660254F; // 30 degrees
    // Motion vectors cannot make a retained frame valid after a
    // camera switch or a discontinuous projection change (FOV,
    // aspect, near/far plane).  Compare the non-zero perspective
    // coefficients rather than relying on position/rotation alone.
    const glm::mat4& projection = currentProjection.native();
    const glm::mat4& previousProjection = previousGameProjection.native();
    const bool projectionChanged =
        previousGameCameraValid && (std::abs(projection[0][0] - previousProjection[0][0]) > 1e-4F ||
                                    std::abs(projection[1][1] - previousProjection[1][1]) > 1e-4F ||
                                    std::abs(projection[2][2] - previousProjection[2][2]) > 1e-4F ||
                                    std::abs(projection[3][2] - previousProjection[3][2]) > 1e-4F);
    const bool cameraCut =
        previousGameCameraValid && ((currentCameraPosition - previousGameCameraPosition).length() > cameraCutDistance ||
                                    dot(currentCameraForward, previousGameCameraForward) < cameraCutDirectionDot ||
                                    activeCamera != previousGameCamera || projectionChanged);
    // Hi-Z tests project bounds into the previous camera view. A pure
    // rotation does not invalidate that depth image: bounds outside
    // its viewport pass the conservative shader checks.
    const bool hiZCameraCut =
        previousGameCameraValid && ((currentCameraPosition - previousGameCameraPosition).length() > cameraCutDistance ||
                                    activeCamera != previousGameCamera || projectionChanged);
    const float cameraPositionDelta = (currentCameraPosition - previousGameCameraPosition).length();
    // Clipmap layout is camera-position/light based, not view-direction
    // based. A yaw/pitch cut keeps its feedback; a large translation
    // does not, since it can invalidate the virtual page coordinates.
    const bool vsmLayoutCut = !previousGameCameraValid || !shadowClipmapsValid || cameraPositionDelta > 8.0F;
    // Bootstrap coarse coverage quickly. Steady-state budgets follow
    // delayed GPU timestamps from completed frame slots.
    const std::uint32_t shadowPageBudget = previousGameCameraValid ? adaptiveShadowPageBudget : 128u;

    std::array<std::uint32_t, ShadowMap::VirtualPageCount> completedVsmRequests{};
    std::span<const std::uint32_t> receiverPageRequests;
    bool receiverFeedbackValid = false;
    // currentFrame's fence was waited before this per-frame update.
    // Reading this mapped allocation therefore consumes GPU work from
    // its prior use with no queue wait or submission-time readback. A
    // view-direction cut does not invalidate shadow page coordinates.
    if (!vsmLayoutCut && vsmRequestsReady[currentFrame] && !vsmRequestsFromScene[currentFrame] &&
        vsmCompactedPageCountBuffers[currentFrame].handle() != VK_NULL_HANDLE) {
        std::uint32_t requestCount{};
        vsmCompactedPageCountBuffers[currentFrame].read(&requestCount, sizeof(requestCount));
        receiverFeedbackValid = true;
        requestCount = std::min(requestCount, ShadowMap::VirtualPageCount);
        if (requestCount != 0) {
            vsmCompactedPageBuffers[currentFrame].read(completedVsmRequests.data(),
                                                       sizeof(std::uint32_t) * requestCount);
            receiverPageRequests = std::span{completedVsmRequests}.first(requestCount);
        }
    }

    if (mainLightShadows && renderGameViewport) {
        shadowClipUpdateMask =
            updateVirtualShadowClipmaps(cameraController.camera()->position(), shadowClipMatrices,
                                        lastShadowCameraPosition, lastShadowLightDirection, shadowClipmapsValid);
        if (receiverFeedbackValid) {
            std::array<std::int32_t, ShadowMap::ClipLevelCount> shiftX{}, shiftY{};
            for (std::uint32_t level = 0; level < ShadowMap::ClipLevelCount; ++level) {
                if (!vsmClipPageShift(vsmRequestClipMatrices[currentFrame][level].native(),
                                      shadowClipMatrices[level].native(), ShadowMap::VirtualPagesPerAxis, shiftX[level],
                                      shiftY[level])) {
                    receiverFeedbackValid = false;
                    receiverPageRequests = {};
                    break;
                }
            }
            if (receiverFeedbackValid) {
                constexpr std::uint32_t axis = ShadowMap::VirtualPagesPerAxis;
                constexpr std::uint32_t pagesPerLevel = axis * axis;
                std::size_t count = 0;
                // Translate delayed keys through whole-page scrolling.
                // Their originating matrices remain attached to the slot,
                // so reused feedback is never translated twice.
                for (const std::uint32_t key : receiverPageRequests) {
                    if (key >= ShadowMap::VirtualPageCount)
                        continue;
                    const std::uint32_t level = key / pagesPerLevel;
                    const std::int32_t x = static_cast<std::int32_t>(key % axis) + shiftX[level];
                    const std::int32_t y = static_cast<std::int32_t>((key % pagesPerLevel) / axis) + shiftY[level];
                    if (x < 0 || y < 0 || x >= static_cast<std::int32_t>(axis) || y >= static_cast<std::int32_t>(axis))
                        continue;
                    completedVsmRequests[count++] =
                        level * pagesPerLevel + static_cast<std::uint32_t>(y) * axis + static_cast<std::uint32_t>(x);
                }
                receiverPageRequests = std::span{completedVsmRequests}.first(count);
            }
        }
        shadowPass.preparePages(shadowClipMatrices,
                                cameraController.camera()->projectionMatrix() * cameraController.camera()->viewMatrix(),
                                gpuObjects, dirtyShadowObjects, receiverPageRequests, receiverFeedbackValid,
                                currentFrame, shadowPageBudget);
    } else {
        shadowClipUpdateMask = 0;
        shadowClipmapsValid = false;
        shadowPass.invalidateCache();
        if (renderGameViewport)
            vsmRequestsReady.fill(false);
    }
    if (vsmPageMarkingUniformBuffers[currentFrame].handle() != VK_NULL_HANDLE) {
        VsmPageMarkingUniforms marking{};
        const auto previousHiZFrame = (currentFrame + MAX_FRAMES_IN_FLIGHT - 1U) % MAX_FRAMES_IN_FLIGHT;
        marking.inverseViewProjection = glm::inverse(hiZViewProjections[previousHiZFrame]);
        for (std::uint32_t level = 0; level < ShadowMap::ClipLevelCount; ++level)
            marking.clipMatrices[level] = shadowClipMatrices[level].native();
        marking.pageCountPerAxis = ShadowMap::VirtualPagesPerAxis;
        marking.virtualResolution = ShadowMap::VirtualResolution;
        marking.clipLevelCount = ShadowMap::ClipLevelCount;
        marking.depthWidth = swapchain.extent().width;
        marking.depthHeight = swapchain.extent().height;
        marking.shadowQuality = static_cast<std::uint32_t>(shadowQuality);
        marking.feedbackStride = 2;
        if (renderGameViewport) {
            vsmPageMarkingUniformBuffers[currentFrame].update(&marking, sizeof(marking));
            const auto& source = hiZBuffers[previousHiZFrame];
            if (source.image() != VK_NULL_HANDLE) {
                const VkDescriptorImageInfo depthInfo{source.sampler(), source.fullView(),
                                                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
                const VkWriteDescriptorSet write{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                                 .dstSet = vsmPageMarkingSets[currentFrame],
                                                 .dstBinding = 0,
                                                 .descriptorCount = 1,
                                                 .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                                 .pImageInfo = &depthInfo};
                vkUpdateDescriptorSets(vulkanDevice.logical(), 1, &write, 0, nullptr);
            }
        }
    }
    // Motion vectors describe continuous motion. Reusing temporal
    // history after a teleport or a large orientation jump produces
    // unavoidable ghosting, so reset those histories at cameraCut.
    if (cameraCut) {
        virtualWaterRenderer.invalidateTemporalHistory();
        // GTAO history is independent of TAA and must not survive a
        // teleport or a large camera rotation either.
        gtaoPass.reset();
        // SSGI reprojects both its radiance and its accumulated GI.
        ssgiPass.reset();
        if (taaResolveActive)
            temporalAaPass.reset();
    }
    if (hiZCameraCut) {
        // A discontinuous camera/projection change invalidates every
        // Hi-Z slot, not only the slot rendered this frame.
        hiZValid.fill(false);
    }
    // SSGI needs the prepass view normals, which MSAA does not write. The
    // decision is made here because Forward reads it from the frame UBO.
    // DDGI is world space and needs no prepass inputs; while it runs it
    // replaces SSGI, which would otherwise count the same bounce twice.
    ddgiTracedThisFrame = renderGameViewport && ddgiSettings.enabled && ddgiPass.available();
    ddgiPass.prepareFrame(frame, cameraController.camera()->position(), ddgiSettings, ddgiTracedThisFrame);
    // Without a valid probe volume Forward never samples the probes; the
    // _noddgi programs also drop the registers that lookup reserves.
    lightingForwardPass.useReducedShaders(!ddgiPass.volumeValid(frame));
    sceneViewportForwardPass.useReducedShaders(!ddgiPass.volumeValid(frame));
    const bool ssgiTracedPreviousFrame = ssgiTracedThisFrame;
    ssgiTracedThisFrame = renderGameViewport && ssgiSettings.quality != SsgiQuality::Off && !msaa.enabled() &&
                          ssgiPass.resultView() != VK_NULL_HANDLE && !ddgiTracedThisFrame;
    // Histories left over from before SSGI was off belong to an older camera.
    if (ssgiTracedThisFrame && !ssgiTracedPreviousFrame)
        ssgiPass.reset();
    // The radiance history was captured with last frame's camera; previous*
    // still holds it until it advances to this frame below.
    ssgiViewToPreviousClip =
        Mat4{(previousGameCameraValid ? previousGameProjection.native() * previousGameView.native()
                                      : currentProjection.native() * currentView.native()) *
             glm::inverse(currentView.native())};
    UniformBufferObject data{
        currentView, currentProjection, Mat4{glm::inverse(currentProjection.native())},
        Mat4{glm::inverse(currentView.native())}, previousGameCameraValid ? previousGameView : currentView,
        previousGameCameraValid ? previousGameProjection : currentProjection, shadowClipMatrices,
        makeShadowClipConstants(shadowClipMatrices),
        Vec4{cameraController.camera()->position().x(), cameraController.camera()->position().y(),
             cameraController.camera()->position().z(), 1.0F},
        Vec4{frameData.directionalLight.direction.x(), frameData.directionalLight.direction.y(),
             frameData.directionalLight.direction.z(), frameData.directionalLight.intensity},
        Vec4{frameData.directionalLight.color.r(), frameData.directionalLight.color.g(),
             frameData.directionalLight.color.b(), 1.0F},
        frameData.wind.directionStrength, frameData.wind.sourcePositionRange, frameData.wind.gustFrequencyTime,
        mainLightShadows ? 1u : 0u, static_cast<std::uint32_t>(shadowQuality),
        static_cast<std::uint32_t>(shadowDebugView), static_cast<std::uint32_t>(gtaoDebugView), materialSlots,
        editorSelectedRenderable, frameData.lightCount, static_cast<std::uint32_t>(reflectionProbes.size()),
        // Bit 0: Game View screen-space inputs are valid. Bit 1: SSGI is
        // traced. Bit 2: TAA resolves this view, so per-pixel dither is safe.
        gameScreenSpaceEffects = 1u | (ssgiTracedThisFrame ? 2u : 0u) | (taaResolveActive ? 4u : 0u),
        // Do not vary PCF/VSM sample phase until TAA has stronger
        // per-surface confidence (normals/reactive mask).  Depth
        // rejection prevents trails, but cannot fully hide changing
        // shadow-filter noise at sub-pixel edges.
        0u, static_cast<std::uint32_t>(Profiler::currentFrameNumber()), static_cast<std::uint32_t>(pbrDebugView),
        glm::vec4{static_cast<float>(swapchain.extent().width), static_cast<float>(swapchain.extent().height), 0.1F,
                  1000.0F},
        glm::vec4{24.0F / std::log2(1000.0F / 0.1F), -std::log2(0.1F) * (24.0F / std::log2(1000.0F / 0.1F)),
                  static_cast<float>(imageBasedLighting.prefilteredMipLevels()),
                  // Finalized after preparing this frame's TLAS,
                  // before the first submission reads the frame UBO.
                  0.0F},
        uniformLocalLights(frameData)};
    uniformBuffers[frame].update(&data, sizeof(data));
    // Game View and Scene View read the same lights from this frame slot.
    localLightBuffers[frame].update(frameData.lights.data(),
                                    sizeof(LocalLightGPU) * std::max(frameData.lightCount, 1U));
    const ClusteredLightingUniforms clustered{
        currentView.native(), currentProjection.native(),
        glm::uvec4{(swapchain.extent().width + ClusterTileSize - 1U) / ClusterTileSize,
                   (swapchain.extent().height + ClusterTileSize - 1U) / ClusterTileSize, ClusterDepthSlices,
                   frameData.lightCount},
        glm::vec4{static_cast<float>(swapchain.extent().width), static_cast<float>(swapchain.extent().height), 0.1F,
                  1000.0F}};
    clusteredLightingUniformBuffers[frame].update(&clustered, sizeof(clustered));
    particlePreviousGameViewProjection =
        previousGameCameraValid ? previousGameProjection * previousGameView : currentProjection * currentView;
    particlePreviousGameCameraRight =
        previousGameCameraValid ? previousGameCameraRight : cameraController.camera()->right();
    particlePreviousGameCameraUp = previousGameCameraValid ? previousGameCameraUp : cameraController.camera()->up();
    previousGameView = currentView;
    previousGameProjection = currentProjection;
    previousGameCamera = activeCamera;
    previousGameCameraPosition = currentCameraPosition;
    previousGameCameraForward = currentCameraForward;
    previousGameCameraRight = cameraController.camera()->right();
    previousGameCameraUp = cameraController.camera()->up();
    previousGameCameraValid = true;
}

void updateSceneViewportUniformBuffer(const uint32_t frame) {
    const SceneFrameData& frameData = sceneFrameDataCache.data;
    const bool mainLightShadows = frameData.directionalLight.enabled && frameData.directionalLight.castShadows &&
                                  optimizationFeatures.shadows && hasShadowCasters;
    const float aspect = static_cast<float>(sceneViewportTarget.extent().width) /
                         static_cast<float>(sceneViewportTarget.extent().height);
    Camera sceneCamera{Degrees{60.0F}, aspect, 0.1F, 1000.0F};
    sceneCamera.setPosition(cameraController.editorPosition());
    sceneCamera.setRotation(Degrees{cameraController.editorYaw()}, Degrees{cameraController.editorPitch()});
    if (mainLightShadows) {
        sceneShadowClipUpdateMask =
            updateVirtualShadowClipmaps(sceneCamera.position(), sceneShadowClipMatrices, lastSceneShadowCameraPosition,
                                        lastSceneShadowLightDirection, sceneShadowClipmapsValid);
        // Consume only completed Scene View work. Game View uses the
        // same frame-slot buffers, but a different camera and page table.
        if (sceneVsmFeedbackPending[currentFrame] && vsmRequestsReady[currentFrame] &&
            vsmRequestsFromScene[currentFrame]) {
            std::uint32_t count{};
            vsmCompactedPageCountBuffers[currentFrame].read(&count, sizeof(count));
            sceneVsmReceiverRequests.resize(std::min(count, ShadowMap::VirtualPageCount));
            if (!sceneVsmReceiverRequests.empty())
                vsmCompactedPageBuffers[currentFrame].read(sceneVsmReceiverRequests.data(),
                                                           sceneVsmReceiverRequests.size() * sizeof(std::uint32_t));
            sceneVsmReceiverMatrices = vsmRequestClipMatrices[currentFrame];
            sceneVsmReceiverFeedbackValid = true;
            sceneVsmFeedbackPending[currentFrame] = false;
        }
        std::vector<std::uint32_t>& requests = vsmRequestScratch;
        requests.clear();
        bool feedbackValid = sceneVsmReceiverFeedbackValid;
        std::array<std::int32_t, ShadowMap::ClipLevelCount> shiftX{}, shiftY{};
        for (std::uint32_t level = 0; feedbackValid && level < ShadowMap::ClipLevelCount; ++level)
            feedbackValid =
                vsmClipPageShift(sceneVsmReceiverMatrices[level].native(), sceneShadowClipMatrices[level].native(),
                                 ShadowMap::VirtualPagesPerAxis, shiftX[level], shiftY[level]);
        if (feedbackValid) {
            constexpr std::uint32_t axis = ShadowMap::VirtualPagesPerAxis;
            constexpr std::uint32_t pagesPerLevel = axis * axis;
            for (const auto key : sceneVsmReceiverRequests) {
                if (key >= ShadowMap::VirtualPageCount)
                    continue;
                const auto level = key / pagesPerLevel;
                const auto x = static_cast<std::int32_t>(key % axis) + shiftX[level];
                const auto y = static_cast<std::int32_t>((key % pagesPerLevel) / axis) + shiftY[level];
                if (x >= 0 && y >= 0 && x < static_cast<std::int32_t>(axis) && y < static_cast<std::int32_t>(axis))
                    requests.push_back(level * pagesPerLevel + y * axis + x);
            }
        }
        sceneDescriptorPass.preparePages(sceneShadowClipMatrices,
                                         sceneCamera.projectionMatrix() * sceneCamera.viewMatrix(), gpuObjects,
                                         dirtyShadowObjects, requests, feedbackValid, currentFrame, 32);
    } else {
        sceneShadowClipUpdateMask = 0;
        sceneShadowClipmapsValid = false;
        sceneDescriptorPass.invalidateCache();
        sceneVsmFeedbackPending.fill(false);
        sceneVsmFeedbackCache = {};
        sceneVsmReceiverFeedbackValid = false;
    }
    const Mat4 sceneView = sceneCamera.viewMatrix();
    const Mat4 sceneProjection = sceneCamera.projectionMatrix();
    sceneVsmFeedbackKey.viewProjection = sceneProjection.native() * sceneView.native();
    // Same depth-relevant revisions as the Game View key (depthContentRevision).
    sceneVsmFeedbackKey.sceneRevision = registry.renderTopologyRevision();
    sceneVsmFeedbackKey.settingsRevision = renderSettingsRevision;
    sceneVsmFeedbackKey.transformRevision = depthContentRevision;
    sceneVsmFeedbackKey.deformationRevision = grassDeformationVersion;
    sceneVsmFeedbackKey.windRevision = sceneFrameDataCache.windRevision;
    sceneVsmFeedbackKey.animationFrame =
        sceneFrameDataCache.hasWind && !sceneGpu.grassInstances.empty() ? submittedFrameValue / 2U : 0;
    sceneVsmFeedbackKey.width = sceneViewportTarget.extent().width;
    sceneVsmFeedbackKey.height = sceneViewportTarget.extent().height;
    sceneVsmFeedbackKey.quality = static_cast<std::uint32_t>(shadowQuality);
    for (std::uint32_t level = 0; level < ShadowMap::ClipLevelCount; ++level)
        sceneVsmFeedbackKey.clipMatrices[level] = sceneShadowClipMatrices[level].native();
    if (vsmPageMarkingUniformBuffers[currentFrame].handle() != VK_NULL_HANDLE) {
        VsmPageMarkingUniforms marking{};
        marking.inverseViewProjection = glm::inverse(sceneVsmFeedbackKey.viewProjection);
        for (std::uint32_t level = 0; level < ShadowMap::ClipLevelCount; ++level)
            marking.clipMatrices[level] = sceneShadowClipMatrices[level].native();
        marking.pageCountPerAxis = ShadowMap::VirtualPagesPerAxis;
        marking.virtualResolution = ShadowMap::VirtualResolution;
        marking.clipLevelCount = ShadowMap::ClipLevelCount;
        marking.depthWidth = sceneVsmFeedbackKey.width;
        marking.depthHeight = sceneVsmFeedbackKey.height;
        marking.shadowQuality = sceneVsmFeedbackKey.quality;
        marking.feedbackStride = 2;
        vsmPageMarkingUniformBuffers[currentFrame].update(&marking, sizeof(marking));
        const VkDescriptorImageInfo depthInfo{sceneViewportDepthBuffer.sampler(), sceneViewportDepthBuffer.imageView(),
                                              VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
        const VkWriteDescriptorSet write{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                         .dstSet = vsmPageMarkingSets[currentFrame],
                                         .dstBinding = 0,
                                         .descriptorCount = 1,
                                         .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                         .pImageInfo = &depthInfo};
        vkUpdateDescriptorSets(vulkanDevice.logical(), 1, &write, 0, nullptr);
    }
    UniformBufferObject data{
        sceneView,
        sceneProjection,
        Mat4{glm::inverse(sceneProjection.native())},
        Mat4{glm::inverse(sceneView.native())},
        sceneView,
        sceneProjection,
        sceneShadowClipMatrices,
        makeShadowClipConstants(sceneShadowClipMatrices),
        Vec4{sceneCamera.position().x(), sceneCamera.position().y(), sceneCamera.position().z(), 1.0F},
        Vec4{frameData.directionalLight.direction.x(), frameData.directionalLight.direction.y(),
             frameData.directionalLight.direction.z(), frameData.directionalLight.intensity},
        Vec4{frameData.directionalLight.color.r(), frameData.directionalLight.color.g(),
             frameData.directionalLight.color.b(), 1.0F},
        frameData.wind.directionStrength,
        frameData.wind.sourcePositionRange,
        frameData.wind.gustFrequencyTime,
        mainLightShadows ? 1u : 0u,
        static_cast<std::uint32_t>(shadowQuality),
        static_cast<std::uint32_t>(shadowDebugView),
        0u,
        materialSlots,
        editorSelectedRenderable,
        frameData.lightCount,
        static_cast<std::uint32_t>(reflectionProbeManager.probes().size()),
        0u,
        0u,
        0u,
        static_cast<std::uint32_t>(pbrDebugView),
        glm::vec4{static_cast<float>(sceneViewportTarget.extent().width),
                  static_cast<float>(sceneViewportTarget.extent().height), 0.1F, 1000.0F},
        glm::vec4{24.0F / std::log2(1000.0F / 0.1F), -std::log2(0.1F) * (24.0F / std::log2(1000.0F / 0.1F)),
                  static_cast<float>(imageBasedLighting.prefilteredMipLevels()), 0.0F},
        uniformLocalLights(frameData)};
    sceneUniformBuffers[frame].update(&data, sizeof(data));
    const ClusteredLightingUniforms clustered{
        sceneView.native(), sceneProjection.native(),
        glm::uvec4{(sceneViewportTarget.extent().width + ClusterTileSize - 1U) / ClusterTileSize,
                   (sceneViewportTarget.extent().height + ClusterTileSize - 1U) / ClusterTileSize, ClusterDepthSlices,
                   frameData.lightCount},
        glm::vec4{static_cast<float>(sceneViewportTarget.extent().width),
                  static_cast<float>(sceneViewportTarget.extent().height), 0.1F, 1000.0F}};
    sceneClusteredLightingUniformBuffers[frame].update(&clustered, sizeof(clustered));
}

struct SwapchainImageIndex final {
    uint32_t value;
};

std::vector<VkCommandBuffer>
prepareGraphCommandBuffers(const RenderGraph::RenderGraph& graph, const uint32_t frame,
                           std::array<std::vector<GraphCommandBuffer>, MAX_FRAMES_IN_FLIGHT>& storage) {
    const auto& batches = graph.queueBatches();
    auto& allocated = storage[frame];
    const auto poolFor = [this](const RenderGraph::Queue queue) {
        const bool useAsyncTransfer = renderPath == RenderPath::AsyncCompute && vulkanDevice.hasAsyncTransferQueue();
        switch (queue) {
        case RenderGraph::Queue::Graphics:
            return commandPool;
        case RenderGraph::Queue::AsyncCompute:
            return vulkanDevice.hasAsyncComputeQueue() ? asyncComputeCommandPool : commandPool;
        case RenderGraph::Queue::Transfer:
            return useAsyncTransfer ? transferCommandPool : commandPool;
        }
        throw std::invalid_argument("Unknown RenderGraph queue");
    };
    bool changed = allocated.size() != batches.size();
    for (std::size_t i = 0; !changed && i < batches.size(); ++i)
        changed = allocated[i].queue != batches[i].queue;
    if (changed) {
        for (const auto& entry : allocated) {
            const VkCommandPool pool = poolFor(entry.queue);
            vkFreeCommandBuffers(device, pool, 1, &entry.buffer);
        }
        allocated.clear();
        for (const auto& batch : batches) {
            const VkCommandPool pool = poolFor(batch.queue);
            if (pool == VK_NULL_HANDLE)
                throw std::runtime_error("RenderGraph queue has no command pool");
            const VkCommandBufferAllocateInfo info{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                                   .commandPool = pool,
                                                   .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                                   .commandBufferCount = 1};
            VkCommandBuffer buffer{};
            if (vkAllocateCommandBuffers(device, &info, &buffer) != VK_SUCCESS)
                throw std::runtime_error("Could not allocate RenderGraph command buffer");
            allocated.push_back({batch.queue, buffer});
        }
    }
    std::vector<VkCommandBuffer> buffers;
    buffers.reserve(allocated.size());
    for (const auto& entry : allocated)
        buffers.push_back(entry.buffer);
    return buffers;
}

void recordCommandBuffer(VkCommandBuffer commandBuffer) {
    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;

    if (vkBeginCommandBuffer(commandBuffer, &beginInfo) != VK_SUCCESS) {
        throw std::runtime_error("Could not begin command buffer");
    }
    gpuTimestampProfiler.beginFrame(commandBuffer, currentFrame);
    const auto profileRenderGraph = [](const RenderGraph::RenderGraph& graph, const bool submitted) {
        const auto stats = graph.statistics();
        Profiler::addRenderGraphStats({.passes = stats.passes,
                                       .graphicsBatches = submitted ? stats.graphicsBatches : 0U,
                                       .computeBatches = submitted ? stats.computeBatches : 0U,
                                       .transferBatches = submitted ? stats.transferBatches : 0U,
                                       .crossQueueEdges = stats.crossQueueEdges,
                                       .pipelineBarriers = stats.pipelineBarriers,
                                       .imageBarriers = stats.imageBarriers,
                                       .bufferBarriers = stats.bufferBarriers,
                                       .ownershipTransfers = stats.ownershipTransfers});
    };
    static const ProfileNameId shadowProfileName = Profiler::registerName("Shadow");
    static const ProfileNameId shadowPageMarkProfileName = Profiler::registerName("Shadow.PageMark");
    static const ProfileNameId shadowPageCompactProfileName = Profiler::registerName("Shadow.PageCompact");
    static const ProfileNameId shadowDepthRasterProfileName = Profiler::registerName("Shadow.CasterCullDepth");
    static const ProfileNameId forwardOpaqueProfileName = Profiler::registerName("Forward.Opaque");
    static const ProfileNameId cullingProfileName = Profiler::registerName("Culling");
    static const ProfileNameId meshletForwardProfileName = Profiler::registerName("Meshlet Forward Prepare");
    static const ProfileNameId meshletInstanceCullProfileName = Profiler::registerName("Meshlet.InstanceCull");
    static const ProfileNameId meshletCullProfileName = Profiler::registerName("Meshlet.Cull");
    static const ProfileNameId meshletBinProfileName = Profiler::registerName("Meshlet.Bin");
    static const ProfileNameId forwardProfileName = Profiler::registerName("Forward");
    static const ProfileNameId depthPrepassProfileName = Profiler::registerName("Depth Prepass");
    static const ProfileNameId hiZProfileName = Profiler::registerName("Hi-Z");
    static const ProfileNameId directionalVisibilityProfileName = Profiler::registerName("Directional Visibility");
    static const ProfileNameId velocityProfileName = Profiler::registerName("Velocity");
    static const ProfileNameId taaProfileName = Profiler::registerName("TAA");
    static const ProfileNameId bloomProfileName = Profiler::registerName("Bloom");
    static const ProfileNameId tonemapProfileName = Profiler::registerName("Tonemap");
    static const ProfileNameId rtTlasProfileName = Profiler::registerName("RT TLAS");
    static const ProfileNameId rtContactProfileName = Profiler::registerName("RT Contact Shadows");
    static const ProfileNameId clusterSceneProfileName = Profiler::registerName("Cluster RT Scene");
    static const ProfileNameId localShadowProfileName = Profiler::registerName("Local Light Shadows");
    static const ProfileNameId localShadowDenoiseProfileName = Profiler::registerName("Local Light Shadow Denoise");
    static const ProfileNameId tileLightCullingProfileName = Profiler::registerName("Tile Light Culling");
    static const ProfileNameId deferredLocalProfileName = Profiler::registerName("Deferred Local Lights");
    static const ProfileNameId ssgiProfileName = Profiler::registerName("SSGI");
    static const ProfileNameId gtaoProfileName = Profiler::registerName("GTAO");
    static const ProfileNameId ddgiProfileName = Profiler::registerName("DDGI");
    static const ProfileNameId ssgiCaptureProfileName = Profiler::registerName("SSGI.Capture");
    const bool renderSceneViewport = editorUiActive && sceneViewportRendered;
    // Scene View replaces the embedded Game View in the editor.  Do
    // not submit hidden Game View work: this also makes the shared
    // physical VSM atlas single-writer for the entire frame.
    const bool renderGameViewport = !editorUiActive || !sceneViewportActive;
    rtContactActive = false;
    // Keep upload synchronization resource-scoped.  This graph is a
    // declaration of every persistent input which can be consumed by
    // the legacy shadow, compute, lighting, post-process, water and
    // Scene View callbacks below.  The callbacks are still being
    // migrated individually, but no upload is allowed to bypass the
    // frame graph and reintroduce a global timeline wait.
    frameGraph.reset();
    frameGraph.enablePassCulling();
    frameGraph.setQueueFamily(RenderGraph::Queue::Graphics, vulkanDevice.graphicsQueueFamily());
    frameGraph.setQueueFamily(RenderGraph::Queue::AsyncCompute, vulkanDevice.computeQueueFamily());
    frameGraph.setQueueFamily(RenderGraph::Queue::Transfer,
                              renderPath == RenderPath::AsyncCompute && vulkanDevice.hasAsyncTransferQueue()
                                  ? vulkanDevice.transferQueueFamily()
                                  : vulkanDevice.graphicsQueueFamily());
    std::vector<RenderGraph::BufferHandle>& frameUploadBuffers = frameUploadBufferScratch;
    frameUploadBuffers.clear();
    const auto importFrameUploadBuffer = [&](const char* name, const Buffer& buffer) {
        if (buffer.handle() == VK_NULL_HANDLE || buffer.size() == 0)
            return;
        const auto handle = frameGraph.importBuffer(
            name, buffer.handle(), {.size = buffer.size(), .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT});
        if (buffer.readyTimeline() != 0)
            frameGraph.markUploaded(handle, buffer.readyTimeline());
        frameUploadBuffers.push_back(handle);
    };
    importFrameUploadBuffer("Geometry vertices", vertexBuffer);
    importFrameUploadBuffer("Geometry indices", indexBuffer);
    importFrameUploadBuffer("Meshlet records", meshletBuffer);
    importFrameUploadBuffer("Meshlet clusters", meshletClusterBuffer);
    importFrameUploadBuffer("Meshlet vertex references", meshletVertexBuffer);
    importFrameUploadBuffer("Meshlet triangles", meshletTriangleBuffer);
    importFrameUploadBuffer("Frame instances", instanceBuffers[currentFrame]);
    importFrameUploadBuffer("Previous transforms", previousTransformBuffers[currentFrame]);
    importFrameUploadBuffer("Frame materials", materialBuffers[currentFrame]);
    importFrameUploadBuffer("GPU scene instances", gpuSceneInstanceBuffers[currentFrame]);
    importFrameUploadBuffer("GPU scene meshes", gpuSceneMeshBuffers[currentFrame]);
    importFrameUploadBuffer("GPU scene materials", gpuSceneMaterialBuffers[currentFrame]);
    importFrameUploadBuffer("Frame lights", uniformBuffers[currentFrame]);
    importFrameUploadBuffer("Shadow culling", shadowCullingUniformBuffers[currentFrame]);
    importFrameUploadBuffer("Clustered lighting", clusteredLightingUniformBuffers[currentFrame]);
    importFrameUploadBuffer("Local lights", localLightBuffers[currentFrame]);
    importFrameUploadBuffer("Scene clustered lighting", sceneClusteredLightingUniformBuffers[currentFrame]);
    if (particleSystem && particleSystem->particleBuffer() != VK_NULL_HANDLE) {
        const auto particleBuffer = frameGraph.importBuffer(
            "Particle state", particleSystem->particleBuffer(),
            {.size = particleSystem->particleBufferSize(), .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT});
        if (particleSystem->readyTimeline() != 0)
            frameGraph.markUploaded(particleBuffer, particleSystem->readyTimeline());
        frameUploadBuffers.push_back(particleBuffer);
    }
    std::vector<RenderGraph::TextureHandle>& frameUploadTextures = frameUploadTextureScratch;
    frameUploadTextures.clear();
    const auto importFrameUploadTexture = [&](const char* name, const Texture2D& texture) {
        if (!texture.valid())
            return;
        const auto handle = frameGraph.importTexture(name, texture.image(),
                                                     {.extent = {texture.width(), texture.height(), 1},
                                                      .format = texture.format(),
                                                      .usage = VK_IMAGE_USAGE_SAMPLED_BIT,
                                                      .aspect = VK_IMAGE_ASPECT_COLOR_BIT,
                                                      .mipLevels = texture.mipLevels()},
                                                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        if (texture.readyTimeline() != 0)
            frameGraph.markUploaded(handle, texture.readyTimeline());
        frameUploadTextures.push_back(handle);
    };
    importFrameUploadTexture("Fallback material", fallbackMaterialTexture);
    importFrameUploadTexture("Grass height", grassHeightTexture);
    importFrameUploadTexture("Grass density", grassDensityTexture);
    for (const auto& [id, resource] : textureGpuResources) {
        (void)id;
        importFrameUploadTexture("Material texture", resource.texture);
    }
    // The command buffers surrounding the viewport and presentation
    // graphs still contain legacy callbacks. Wait only for resources
    // those callbacks can bind, never for an unrelated transfer on the
    // shared upload timeline. Keep this in sync with the imports above.
    const auto legacyUploadValue = [&]() {
        std::uint64_t value = 0;
        const std::array<const Buffer*, 15> buffers{&vertexBuffer,
                                                    &indexBuffer,
                                                    &instanceBuffers[currentFrame],
                                                    &previousTransformBuffers[currentFrame],
                                                    &materialBuffers[currentFrame],
                                                    &gpuSceneInstanceBuffers[currentFrame],
                                                    &gpuSceneMeshBuffers[currentFrame],
                                                    &gpuSceneMaterialBuffers[currentFrame],
                                                    &uniformBuffers[currentFrame],
                                                    &shadowCullingUniformBuffers[currentFrame],
                                                    &clusteredLightingUniformBuffers[currentFrame],
                                                    &meshletBuffer,
                                                    &meshletClusterBuffer,
                                                    &meshletVertexBuffer,
                                                    &meshletTriangleBuffer};
        for (const Buffer* buffer : buffers)
            if (buffer->handle() != VK_NULL_HANDLE)
                value = std::max(value, buffer->readyTimeline());
        value = std::max(value, sceneClusteredLightingUniformBuffers[currentFrame].readyTimeline());
        if (particleSystem && particleSystem->particleBuffer() != VK_NULL_HANDLE)
            value = std::max(value, particleSystem->readyTimeline());
        const auto includeTexture = [&](const Texture2D& texture) {
            if (texture.valid())
                value = std::max(value, texture.readyTimeline());
        };
        includeTexture(fallbackMaterialTexture);
        includeTexture(grassHeightTexture);
        includeTexture(grassDensityTexture);
        for (const auto& [id, resource] : textureGpuResources) {
            (void)id;
            includeTexture(resource.texture);
        }
        return value;
    };
    VkPipelineStageFlags2 legacyUploadStages = VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT |
                                               VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                                               VK_PIPELINE_STAGE_2_TRANSFER_BIT | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT;
    if (vulkanDevice.supportsRayQuery())
        legacyUploadStages |= VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
    frameGraph.addPass(
        "Legacy frame resource consumers", RenderGraph::Queue::Graphics,
        [&](RenderGraph::PassBuilder& builder) {
            // The callbacks that bind these descriptor sets are not
            // graph-owned yet. Keep their resource-scoped upload wait
            // as an explicit culling root until that migration ends.
            builder.setSideEffect();
            // ALL_COMMANDS is intentional: these resources are bound
            // by descriptor sets owned by still-legacy callbacks, so
            // their first precise stage has not yet been split out.
            for (const auto buffer : frameUploadBuffers)
                builder.read(buffer, RenderGraph::BufferUsage::ExternalRead);
            for (const auto texture : frameUploadTextures)
                builder.read(texture, RenderGraph::TextureUsage::ExternalRead);
        },
        [](VkCommandBuffer) {});
    // TAA consumes the Virtual Water prepass attachments.  Keep this
    // frame-local contract separate from whether the water world has
    // bodies, as a renderer can be active without its prepass having
    // been recorded yet.
    // The virtual-water prepass is recorded by the legacy work pass
    // below whenever the Game View and the renderer are active. Keep
    // this decision available while declaring TAA's graph resources.
    // Set only once the water prepass is actually recorded: TAA and the frame
    // graph then sample the water targets, which must have left UNDEFINED.
    bool virtualWaterPreparedThisFrame = false;
    // Record legacy shadowing, culling, lighting and Scene View before
    // submitting the presentation graph. The viewport graph may split
    // this work across graphics and compute submissions.
    if (displacedMaterialsDirty) {
        // Same test as materialHasDisplacement() in Displacement.slang.
        const bool noDisplacement = std::ranges::none_of(materials, [](const GPUMaterialData& material) {
            return material.auxiliaryTextureIndices.w >= 0 &&
                   (std::abs(material.displacementParams.x) > 1.0e-6F ||
                    std::abs(material.displacementParams.y) > 1.0e-6F);
        });
        forwardPass.useReducedShaders(noDisplacement);
        sceneViewportDepthPrepass.useReducedShaders(noDisplacement);
        lightingForwardPass.useFlatMeshShaders(noDisplacement);
        sceneViewportForwardPass.useFlatMeshShaders(noDisplacement);
        materialsDisplaced = !noDisplacement;
        displacedMaterialsDirty = false;
    }
    {
        bool meshletForwardReady = meshletForwardPathActive && renderGameViewport &&
                                   vulkanDevice.supportsMeshShaders() && optimizationFeatures.gpuCulling &&
                                   globalMeshletCount != 0 &&
                                   // Visible-meshlet entries pack the id into 24 bits.
                                   globalMeshletCount <= 0x00ffffffU &&
                                   meshletIndexedDrawsSupported() &&
                                   !instanceBatches.empty() && !sceneGpu.database.instances().empty() &&
                                   meshletCullSets[currentFrame] != VK_NULL_HANDLE;
        // Foliage-pipeline batches (two-sided, masked or blended) already
        // own a dedicated indexed stream. When one of them cannot use the
        // meshlet commands, keep that whole stream indexed and route only
        // the remaining opaque geometry through meshlet culling, instead of
        // giving up cluster culling for the entire scene.
        bool meshletSkipsFoliage = false;
        // Rasterize the visible meshlets with mesh shaders (one workgroup
        // per meshlet) rather than one indexed draw per meshlet run.
        const bool meshShaderCapable =
            optimizationFeatures.meshShaders && forwardPass.hasMeshShaderPipelines() &&
            lightingForwardPass.hasMeshShaderPipelines() && meshletDrawBinPipeline != VK_NULL_HANDLE &&
            meshletDrawSets[currentFrame] != VK_NULL_HANDLE &&
            meshletDrawListBuffers[currentFrame].handle() != VK_NULL_HANDLE;
        if (meshletForwardReady) {
            for (const InstanceBatch& batch : instanceBatches) {
                if (isCompositedProgramSlot(batch.shaderSlot))
                    continue;
                // Indexed meshlet commands mix both face modes in one
                // back-face-culled shader bin, so two-sided materials need
                // the indexed lists there. Mesh shaders cull per primitive.
                // Shader Graph slots only have mesh-shader meshlet pipelines.
                const bool meshletPipeline =
                    batch.shaderSlot < MaterialShaderCount ||
                    (meshShaderCapable && forwardPass.hasMeshShaderPipeline(batch.shaderSlot) &&
                     lightingForwardPass.hasMeshShaderPipeline(batch.shaderSlot));
                if (batch.meshletCount != 0 && meshletPipeline && (meshShaderCapable || !batch.twoSided) &&
                    batch.lod1IndexCount == 0 && batch.lod2IndexCount == 0)
                    continue;
                if (batch.foliagePipeline) {
                    meshletSkipsFoliage = true;
                    continue;
                }
                meshletForwardReady = false;
                meshletSkipsFoliage = false;
                break;
            }
        }
        const bool meshShaderForward = meshletForwardReady && meshShaderCapable;
        { static int d = 0; if (d++ == 150) std::fprintf(stderr, "TEMP-DEBUG ready=%d mesh=%d skipsFoliage=%d\n", (int)meshletForwardReady, (int)meshShaderForward, (int)meshletSkipsFoliage); }
        // Culling samples the previous frame's pyramid while this frame's
        // depth is reduced into a different image.
        // Empty scenes do not allocate culling/Hi-Z resources. Keep the
        // frame path disabled for them so no barrier references the null
        // image handle left by the intentionally skipped allocation.
        const auto& hiZBuffer = hiZBuffers[currentFrame];
        const auto previousHiZFrame = (currentFrame + MAX_FRAMES_IN_FLIGHT - 1U) % MAX_FRAMES_IN_FLIGHT;
        const auto& previousHiZBuffer = hiZBuffers[previousHiZFrame];
        const bool hasHiZResources = hiZBuffer.image() != VK_NULL_HANDLE;
        const bool hadPreviousHiZ = hiZValid[previousHiZFrame];
        const DirectionalLight& mainLight = sceneFrameDataCache.data.directionalLight;
        const bool mainLightShadows =
            mainLight.enabled && mainLight.castShadows && optimizationFeatures.shadows && hasShadowCasters;
        // Shadows need receiver depth even when occlusion culling is off.
        const bool hizEnabled =
            hasHiZResources && (canUseHiZOcclusionCulling() || (mainLightShadows && renderGameViewport));

        if (meshletForwardReady) {
            const std::uint32_t excludeFoliagePipeline = meshletSkipsFoliage ? 1U : 0U;
            meshletCullingUniformBuffers[currentFrame].update(
                &excludeFoliagePipeline, sizeof(excludeFoliagePipeline),
                offsetof(Culling::MeshletCullUniforms, excludeFoliagePipeline));
            gpuTimestampProfiler.beginZone(commandBuffer, currentFrame, meshletForwardProfileName);
            // Build an indirect command from the compact coarse-visible
            // list, so meshlet culling launches one group per visible
            // instance instead of one per instance in the whole scene.
            gpuTimestampProfiler.beginZone(commandBuffer, currentFrame, meshletInstanceCullProfileName);
            instanceCullingPasses[currentFrame].record(
                commandBuffer, static_cast<std::uint32_t>(sceneGpu.database.instances().size()));
            gpuTimestampProfiler.endZone(commandBuffer, currentFrame);
            vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, meshletDispatchPipeline);
            const VkDescriptorSet meshletDispatchSet = meshletDispatchSets[currentFrame];
            vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, meshletDispatchPipelineLayout, 0, 1,
                                    &meshletDispatchSet, 0, nullptr);
            vkCmdDispatch(commandBuffer, 1, 1, 1);
            const VkBufferMemoryBarrier2 meshletDispatchBarrier{.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
                                                                .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                                                .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                                                                .dstStageMask = VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT,
                                                                .dstAccessMask = VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT,
                                                                .buffer =
                                                                    meshletCullDispatchBuffers[currentFrame].handle(),
                                                                .offset = 0,
                                                                .size = sizeof(VkDispatchIndirectCommand)};
            const VkDependencyInfo meshletDispatchDependency{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                                                             .bufferMemoryBarrierCount = 1,
                                                             .pBufferMemoryBarriers = &meshletDispatchBarrier};
            vkCmdPipelineBarrier2(commandBuffer, &meshletDispatchDependency);
            vkCmdFillBuffer(commandBuffer, visibleMeshletCountBuffers[currentFrame].handle(), 0, sizeof(std::uint32_t),
                            0);
            const VkMemoryBarrier2 clearBarrier{
                VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,     nullptr,
                VK_PIPELINE_STAGE_2_TRANSFER_BIT,       VK_ACCESS_2_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT};
            const VkDependencyInfo clearDependency{
                .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &clearBarrier};
            vkCmdPipelineBarrier2(commandBuffer, &clearDependency);
            gpuTimestampProfiler.beginZone(commandBuffer, currentFrame, meshletCullProfileName);
            vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, meshletCullingPipeline);
            const VkDescriptorSet meshletSet = meshletCullSets[currentFrame];
            vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, meshletCullingPipelineLayout, 0, 1,
                                    &meshletSet, 0, nullptr);
            vkCmdDispatchIndirect(commandBuffer, meshletCullDispatchBuffers[currentFrame].handle(), 0);
            gpuTimestampProfiler.endZone(commandBuffer, currentFrame);
            const VkMemoryBarrier2 meshletBarrier{
                VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,     nullptr,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT};
            const VkDependencyInfo meshletDependency{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                                                     .memoryBarrierCount = 1,
                                                     .pMemoryBarriers = &meshletBarrier};
            vkCmdPipelineBarrier2(commandBuffer, &meshletDependency);
            gpuTimestampProfiler.beginZone(commandBuffer, currentFrame, meshletBinProfileName);
            vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                              meshShaderForward ? meshletDrawBinPipeline : meshletIndirectPipeline);
            vkCmdFillBuffer(commandBuffer, meshletIndexedDrawCountBuffers[currentFrame].handle(), 0,
                            sizeof(std::uint32_t) * MaterialProgramSlotCount, 0);
            const VkMemoryBarrier2 meshletCountClearBarrier{
                VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                nullptr,
                VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                VK_ACCESS_2_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT};
            const VkDependencyInfo meshletCountClearDependency{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                                                               .memoryBarrierCount = 1,
                                                               .pMemoryBarriers = &meshletCountClearBarrier};
            vkCmdPipelineBarrier2(commandBuffer, &meshletCountClearDependency);
            const VkDescriptorSet meshletIndirectSet =
                meshShaderForward ? meshletDrawBinSets[currentFrame] : meshletIndirectSets[currentFrame];
            vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, meshletIndirectPipelineLayout, 0, 1,
                                    &meshletIndirectSet, 0, nullptr);
            // BinGroups in meshlet_build_mesh_draws / meshlet_build_indexed:
            // both loop over the visible count.
            constexpr std::uint32_t meshletBinGroups = 1024U;
            vkCmdDispatch(commandBuffer, std::min(meshletBinGroups, (meshletVisibleCapacity + 63U) / 64U), 1, 1);
            if (meshShaderForward) {
                // Per-slot counts are final: derive the mesh-task commands.
                vkCmdPipelineBarrier2(commandBuffer, &meshletDependency);
                vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, meshletDrawCommandPipeline);
                const VkDescriptorSet meshletDrawCommandSet = meshletDrawCommandSets[currentFrame];
                vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, meshletDispatchPipelineLayout,
                                        0, 1, &meshletDrawCommandSet, 0, nullptr);
                vkCmdDispatch(commandBuffer, 1, 1, 1);
            }
            gpuTimestampProfiler.endZone(commandBuffer, currentFrame);
            // The mesh stage also reads the entry list and counts as storage.
            const VkMemoryBarrier2 meshTaskIndirectBarrier{
                VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                nullptr,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT |
                    (meshShaderForward ? VK_PIPELINE_STAGE_2_MESH_SHADER_BIT_EXT : VK_PIPELINE_STAGE_2_NONE),
                VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT |
                    (meshShaderForward ? VK_ACCESS_2_SHADER_STORAGE_READ_BIT : VK_ACCESS_2_NONE)};
            const VkDependencyInfo meshTaskIndirectDependency{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                                                              .memoryBarrierCount = 1,
                                                              .pMemoryBarriers = &meshTaskIndirectBarrier};
            vkCmdPipelineBarrier2(commandBuffer, &meshTaskIndirectDependency);
            gpuTimestampProfiler.endZone(commandBuffer, currentFrame);
        }
        const ForwardPass& sceneForwardPass = sceneViewportForwardPass;
        // ImGui owns a persistent descriptor for Scene View and may sample
        // it even before the viewport receives its first deferred redraw.
        // The first use may be a real Scene View render, so prepare the
        // color target for either that render or the clear below.
        if (!sceneViewportImageInitialized) {
            VkImageMemoryBarrier2 initializeColor{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
            initializeColor.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
            initializeColor.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
            initializeColor.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            initializeColor.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            initializeColor.image = sceneViewportTarget.color().image();
            initializeColor.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            dependency.imageMemoryBarrierCount = 1;
            dependency.pImageMemoryBarriers = &initializeColor;
            vkCmdPipelineBarrier2(commandBuffer, &dependency);
        }
        // Water's immutable opaque-color descriptor is created along with
        // the render target, before it has received its first copy. Keep
        // the descriptor's declared layout valid even on frames that do
        // not render water; the later copy overwrites its contents before
        // the image is sampled.
        if (!opaqueSceneColorInitialized) {
            VkImageMemoryBarrier2 initializeOpaqueColor{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
            initializeOpaqueColor.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
            initializeOpaqueColor.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
            initializeOpaqueColor.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            initializeOpaqueColor.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            initializeOpaqueColor.image = opaqueSceneColor.image();
            initializeOpaqueColor.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            dependency.imageMemoryBarrierCount = 1;
            dependency.pImageMemoryBarriers = &initializeOpaqueColor;
            vkCmdPipelineBarrier2(commandBuffer, &dependency);
            opaqueSceneColorInitialized = true;
        }
        if (!renderSceneViewport && !sceneViewportImageInitialized) {
            VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
            color.imageView = sceneViewportTarget.color().imageView();
            color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            color.clearValue.color = {{0.02F, 0.02F, 0.05F, 1.0F}};
            VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
            rendering.renderArea.extent = sceneViewportTarget.extent();
            rendering.layerCount = 1;
            rendering.colorAttachmentCount = 1;
            rendering.pColorAttachments = &color;
            vkCmdBeginRendering(commandBuffer, &rendering);
            vkCmdEndRendering(commandBuffer);
            VkImageMemoryBarrier2 colorToSampled{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
            colorToSampled.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
            colorToSampled.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
            colorToSampled.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
            colorToSampled.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
            colorToSampled.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            colorToSampled.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            colorToSampled.image = sceneViewportTarget.color().image();
            colorToSampled.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            dependency.imageMemoryBarrierCount = 1;
            dependency.pImageMemoryBarriers = &colorToSampled;
            vkCmdPipelineBarrier2(commandBuffer, &dependency);
            sceneViewportImageInitialized = true;
        }

        if (particleSystem) {
            particleSystem->recordCompute(commandBuffer, particleComputePipeline, particleComputePipelineLayout,
                                          currentFrame);
        }

        // Packed grass has an independent, fully GPU-resident command path.
        // Each classify stream is compacted by cluster before its indirect
        // commands are emitted, so mixed grass meshes never share a draw.
        // The shadow stream is also consumed by Scene View, which has no
        // dedicated grass-shadow stream yet. Keep this producer alive
        // even when the hidden Game View forward pass is omitted.
        if (!sceneGpu.grassInstances.empty() && cameraController.camera()) {
            auto& lists = grassRenderLists[currentFrame];
            const auto camera = cameraController.camera();
            GrassPackedCullUniformData cullData{};
            cullData.viewProjection = camera->unjitteredProjectionMatrix().native() * camera->viewMatrix().native();
            cullData.cameraPosition = glm::vec4{camera->position().native(), 1.0F};
            cullData.clusterCount = static_cast<uint32_t>(sceneGpu.grassClusters.size());
            cullData.frustumPlanes = extractFrustumPlanes(cullData.viewProjection);
            grassPackedCullUniformBuffers[currentFrame].update(&cullData, sizeof(cullData));
            const GrassClassifyUniformData classifyData{grassSettings.renderDistance, grassSettings.shadowDistance,
                                                        grassSettings.velocityDistance, 0.0F,
                                                        glm::vec4{camera->position().native(), 1.0F}};
            grassClassifyUniformBuffers[currentFrame].update(&classifyData, sizeof(classifyData));
            const GrassPackedStreamUniformData streamData{static_cast<uint32_t>(sceneGpu.grassInstances.size()), 0U,
                                                          static_cast<uint32_t>(sceneGpu.grassClusters.size()), 0U};
            for (uint32_t stream = 0; stream < 3; ++stream) {
                auto data = streamData;
                data.streamIndex = stream;
                grassPackedStreamUniformBuffers[currentFrame][stream].update(&data, sizeof(data));
            }
            vkCmdFillBuffer(commandBuffer, lists.visibleCount.handle(), 0, VK_WHOLE_SIZE, 0);
            vkCmdFillBuffer(commandBuffer, lists.visibleClusterCount.handle(), 0, VK_WHOLE_SIZE, 0);
            vkCmdFillBuffer(commandBuffer, lists.bladeCullDispatch.handle(), sizeof(uint32_t), sizeof(uint32_t), 0);
            vkCmdFillBuffer(commandBuffer, lists.classifyCounts.handle(), 0, VK_WHOLE_SIZE, 0);
            for (uint32_t stream = 0; stream < 3; ++stream) {
                vkCmdFillBuffer(commandBuffer, lists.binCounts[stream].handle(), 0, VK_WHOLE_SIZE, 0);
                vkCmdFillBuffer(commandBuffer, lists.binCursors[stream].handle(), 0, VK_WHOLE_SIZE, 0);
            }
            vkCmdFillBuffer(commandBuffer, lists.mainDrawCount.handle(), 0, sizeof(uint32_t), 0);
            vkCmdFillBuffer(commandBuffer, lists.shadowDrawCount.handle(), 0, sizeof(uint32_t), 0);
            vkCmdFillBuffer(commandBuffer, lists.velocityDrawCount.handle(), 0, sizeof(uint32_t), 0);
            const VkMemoryBarrier2 transferToCompute{
                VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                nullptr,
                VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                VK_ACCESS_2_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT};
            const VkDependencyInfo dependency{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                                              .memoryBarrierCount = 1,
                                              .pMemoryBarriers = &transferToCompute};
            vkCmdPipelineBarrier2(commandBuffer, &dependency);
            const auto dispatch = [&](VkPipeline pipeline, VkPipelineLayout layout, VkDescriptorSet set,
                                      uint32_t groups) {
                vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
                vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, nullptr);
                vkCmdDispatch(commandBuffer, groups, 1, 1);
                const VkMemoryBarrier2 barrier{
                    VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                    nullptr,
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                    VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                    VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT};
                const VkDependencyInfo info{
                    .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &barrier};
                vkCmdPipelineBarrier2(commandBuffer, &info);
            };
            dispatch(grassPackedCullPipeline, grassPackedCullPipelineLayout, grassPackedCullSets[currentFrame],
                     (static_cast<uint32_t>(sceneGpu.grassClusters.size()) + 63U) / 64U);
            const VkMemoryBarrier2 clusterDispatchBarrier{
                VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,     nullptr,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT,  VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT};
            const VkDependencyInfo clusterDispatchInfo{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                                                       .memoryBarrierCount = 1,
                                                       .pMemoryBarriers = &clusterDispatchBarrier};
            vkCmdPipelineBarrier2(commandBuffer, &clusterDispatchInfo);
            vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, grassBladeCullPipeline);
            const VkDescriptorSet bladeSet = grassBladeCullSets[currentFrame];
            vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, grassBladeCullPipelineLayout, 0, 1,
                                    &bladeSet, 0, nullptr);
            vkCmdDispatchIndirect(commandBuffer, lists.bladeCullDispatch.handle(), 0);
            const VkMemoryBarrier2 bladeBarrier{
                VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                nullptr,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT};
            const VkDependencyInfo bladeInfo{
                .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &bladeBarrier};
            vkCmdPipelineBarrier2(commandBuffer, &bladeInfo);
            dispatch(grassDispatchBuildPipeline, grassDispatchBuildPipelineLayout,
                     grassVisibleDispatchBuildSets[currentFrame], 1);
            const VkMemoryBarrier2 dispatchBuildBarrier{
                VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,     nullptr,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT,  VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT};
            const VkDependencyInfo dispatchBuildInfo{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                                                     .memoryBarrierCount = 1,
                                                     .pMemoryBarriers = &dispatchBuildBarrier};
            vkCmdPipelineBarrier2(commandBuffer, &dispatchBuildInfo);
            vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, grassClassifyPipeline);
            const VkDescriptorSet classifySet = grassClassifySets[currentFrame];
            vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, grassClassifyPipelineLayout, 0, 1,
                                    &classifySet, 0, nullptr);
            vkCmdDispatchIndirect(commandBuffer, lists.dispatchIndirect.handle(), 0);
            vkCmdPipelineBarrier2(commandBuffer, &bladeInfo);
            dispatch(grassDispatchBuildPipeline, grassDispatchBuildPipelineLayout,
                     grassStreamDispatchBuildSets[currentFrame], 1);
            vkCmdPipelineBarrier2(commandBuffer, &dispatchBuildInfo);
            for (uint32_t stream = 0; stream < 3; ++stream) {
                const VkDeviceSize dispatchOffset = sizeof(GrassBladeDispatchData) * stream;
                vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, grassPackedBinPipeline);
                const VkDescriptorSet binSet = grassPackedBinSets[currentFrame][stream];
                vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, grassPackedBinPipelineLayout, 0,
                                        1, &binSet, 0, nullptr);
                vkCmdDispatchIndirect(commandBuffer, lists.dispatchIndirect.handle(), dispatchOffset);
                vkCmdPipelineBarrier2(commandBuffer, &bladeInfo);
                dispatch(grassPackedPrefixPipeline, grassPrefixPipelineLayout,
                         grassPackedPrefixSets[currentFrame][stream], 1);
                vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, grassPackedScatterPipeline);
                const VkDescriptorSet scatterSet = grassPackedScatterSets[currentFrame][stream];
                vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, grassPackedScatterPipelineLayout,
                                        0, 1, &scatterSet, 0, nullptr);
                vkCmdDispatchIndirect(commandBuffer, lists.dispatchIndirect.handle(), dispatchOffset);
                vkCmdPipelineBarrier2(commandBuffer, &bladeInfo);
                dispatch(grassPackedFinalizePipeline, grassPackedFinalizePipelineLayout,
                         grassPackedFinalizeSets[currentFrame][stream],
                         (static_cast<uint32_t>(sceneGpu.grassClusters.size()) + 63U) / 64U);
            }
            const VkMemoryBarrier2 grassDrawBarrier{
                VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                nullptr,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
                VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT};
            const VkDependencyInfo grassDrawDependency{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                                                       .memoryBarrierCount = 1,
                                                       .pMemoryBarriers = &grassDrawBarrier};
            vkCmdPipelineBarrier2(commandBuffer, &grassDrawDependency);
        }

        // The editor camera has an independent frustum.  Produce its own
        // packed-grass command list instead of reusing Game View results.
        if (renderSceneViewport && !sceneGpu.grassInstances.empty()) {
            auto& lists = sceneGrassRenderLists[currentFrame];
            const float aspect = static_cast<float>(sceneViewportTarget.extent().width) /
                                 static_cast<float>(sceneViewportTarget.extent().height);
            Camera sceneCamera{Degrees{60.0F}, aspect, 0.1F, 1000.0F};
            sceneCamera.setPosition(cameraController.editorPosition());
            sceneCamera.setRotation(Degrees{cameraController.editorYaw()}, Degrees{cameraController.editorPitch()});
            GrassPackedCullUniformData cullData{};
            cullData.viewProjection =
                sceneCamera.unjitteredProjectionMatrix().native() * sceneCamera.viewMatrix().native();
            cullData.cameraPosition = glm::vec4{sceneCamera.position().native(), 1.0F};
            cullData.clusterCount = static_cast<uint32_t>(sceneGpu.grassClusters.size());
            cullData.frustumPlanes = extractFrustumPlanes(cullData.viewProjection);
            sceneGrassPackedCullUniformBuffers[currentFrame].update(&cullData, sizeof(cullData));
            const GrassClassifyUniformData classifyData{grassSettings.renderDistance, grassSettings.shadowDistance,
                                                        grassSettings.velocityDistance, 0.0F,
                                                        glm::vec4{sceneCamera.position().native(), 1.0F}};
            sceneGrassClassifyUniformBuffers[currentFrame].update(&classifyData, sizeof(classifyData));
            const GrassPackedStreamUniformData streamData{static_cast<uint32_t>(sceneGpu.grassInstances.size()), 0U,
                                                          static_cast<uint32_t>(sceneGpu.grassClusters.size()), 0U};
            for (uint32_t stream = 0; stream < 3; ++stream) {
                auto data = streamData;
                data.streamIndex = stream;
                sceneGrassPackedStreamUniformBuffers[currentFrame][stream].update(&data, sizeof(data));
            }
            vkCmdFillBuffer(commandBuffer, lists.visibleCount.handle(), 0, VK_WHOLE_SIZE, 0);
            vkCmdFillBuffer(commandBuffer, lists.visibleClusterCount.handle(), 0, VK_WHOLE_SIZE, 0);
            vkCmdFillBuffer(commandBuffer, lists.bladeCullDispatch.handle(), sizeof(uint32_t), sizeof(uint32_t), 0);
            vkCmdFillBuffer(commandBuffer, lists.classifyCounts.handle(), 0, VK_WHOLE_SIZE, 0);
            vkCmdFillBuffer(commandBuffer, lists.binCounts[0].handle(), 0, VK_WHOLE_SIZE, 0);
            vkCmdFillBuffer(commandBuffer, lists.binCursors[0].handle(), 0, VK_WHOLE_SIZE, 0);
            vkCmdFillBuffer(commandBuffer, lists.mainDrawCount.handle(), 0, sizeof(uint32_t), 0);
            const VkMemoryBarrier2 transferToCompute{
                VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                nullptr,
                VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                VK_ACCESS_2_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT};
            const VkDependencyInfo dependency{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                                              .memoryBarrierCount = 1,
                                              .pMemoryBarriers = &transferToCompute};
            vkCmdPipelineBarrier2(commandBuffer, &dependency);
            const auto dispatch = [&](VkPipeline pipeline, VkPipelineLayout layout, VkDescriptorSet set,
                                      uint32_t groups) {
                vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
                vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, nullptr);
                vkCmdDispatch(commandBuffer, groups, 1, 1);
                const VkMemoryBarrier2 barrier{
                    VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                    nullptr,
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                    VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                    VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT};
                const VkDependencyInfo info{
                    .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &barrier};
                vkCmdPipelineBarrier2(commandBuffer, &info);
            };
            dispatch(grassPackedCullPipeline, grassPackedCullPipelineLayout, sceneGrassPackedCullSets[currentFrame],
                     (static_cast<uint32_t>(sceneGpu.grassClusters.size()) + 63U) / 64U);
            const VkMemoryBarrier2 sceneClusterDispatchBarrier{
                VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,     nullptr,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT,  VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT};
            const VkDependencyInfo sceneClusterDispatchInfo{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                                                            .memoryBarrierCount = 1,
                                                            .pMemoryBarriers = &sceneClusterDispatchBarrier};
            vkCmdPipelineBarrier2(commandBuffer, &sceneClusterDispatchInfo);
            vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, grassBladeCullPipeline);
            const VkDescriptorSet sceneBladeSet = sceneGrassBladeCullSets[currentFrame];
            vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, grassBladeCullPipelineLayout, 0, 1,
                                    &sceneBladeSet, 0, nullptr);
            vkCmdDispatchIndirect(commandBuffer, lists.bladeCullDispatch.handle(), 0);
            // vkCmdDispatchIndirect is not followed by the helper above;
            // make its visible-blade list available to Scene classify.
            const VkMemoryBarrier2 sceneBladeBarrier{
                VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                nullptr,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT};
            const VkDependencyInfo sceneBladeInfo{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                                                  .memoryBarrierCount = 1,
                                                  .pMemoryBarriers = &sceneBladeBarrier};
            vkCmdPipelineBarrier2(commandBuffer, &sceneBladeInfo);
            dispatch(grassDispatchBuildPipeline, grassDispatchBuildPipelineLayout,
                     sceneGrassVisibleDispatchBuildSets[currentFrame], 1);
            const VkMemoryBarrier2 sceneDispatchBuildBarrier{
                VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,     nullptr,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT,  VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT};
            const VkDependencyInfo sceneDispatchBuildInfo{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                                                          .memoryBarrierCount = 1,
                                                          .pMemoryBarriers = &sceneDispatchBuildBarrier};
            vkCmdPipelineBarrier2(commandBuffer, &sceneDispatchBuildInfo);
            vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, grassClassifyPipeline);
            const VkDescriptorSet sceneClassifySet = sceneGrassClassifySets[currentFrame];
            vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, grassClassifyPipelineLayout, 0, 1,
                                    &sceneClassifySet, 0, nullptr);
            vkCmdDispatchIndirect(commandBuffer, lists.dispatchIndirect.handle(), 0);
            vkCmdPipelineBarrier2(commandBuffer, &sceneBladeInfo);
            dispatch(grassDispatchBuildPipeline, grassDispatchBuildPipelineLayout,
                     sceneGrassStreamDispatchBuildSets[currentFrame], 1);
            vkCmdPipelineBarrier2(commandBuffer, &sceneDispatchBuildInfo);
            // Scene View consumes only the main stream; its shadow and velocity
            // streams are intentionally left unused.
            constexpr uint32_t stream = 0;
            vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, grassPackedBinPipeline);
            const VkDescriptorSet sceneBinSet = sceneGrassPackedBinSets[currentFrame][stream];
            vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, grassPackedBinPipelineLayout, 0, 1,
                                    &sceneBinSet, 0, nullptr);
            vkCmdDispatchIndirect(commandBuffer, lists.dispatchIndirect.handle(), 0);
            vkCmdPipelineBarrier2(commandBuffer, &sceneBladeInfo);
            dispatch(grassPackedPrefixPipeline, grassPrefixPipelineLayout,
                     sceneGrassPackedPrefixSets[currentFrame][stream], 1);
            vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, grassPackedScatterPipeline);
            const VkDescriptorSet sceneScatterSet = sceneGrassPackedScatterSets[currentFrame][stream];
            vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, grassPackedScatterPipelineLayout, 0,
                                    1, &sceneScatterSet, 0, nullptr);
            vkCmdDispatchIndirect(commandBuffer, lists.dispatchIndirect.handle(), 0);
            vkCmdPipelineBarrier2(commandBuffer, &sceneBladeInfo);
            dispatch(grassPackedFinalizePipeline, grassPackedFinalizePipelineLayout,
                     sceneGrassPackedFinalizeSets[currentFrame][stream],
                     (static_cast<uint32_t>(sceneGpu.grassClusters.size()) + 63U) / 64U);
            const VkMemoryBarrier2 grassDrawBarrier{
                VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                nullptr,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
                VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT};
            const VkDependencyInfo grassDrawDependency{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                                                       .memoryBarrierCount = 1,
                                                       .pMemoryBarriers = &grassDrawBarrier};
            vkCmdPipelineBarrier2(commandBuffer, &grassDrawDependency);
        }

        // The packed shadow stream is now complete and visible to indirect
        // draws. Bind it before recording either shadow atlas: an earlier
        // command-buffer order made grass_shadow consume the previous frame.
        Culling::IndexedIndirectDrawCount grassShadowDraw;
        const Culling::IndexedIndirectDrawCount* grassShadowDrawPtr = nullptr;
        if (!sceneGpu.grassInstances.empty()) {
            const auto& lists = grassRenderLists[currentFrame];
            // Build the real cluster -> VSM-page overlap stream. The
            // camera-wide shadow list only limits distance; it must never
            // be drawn unchanged into every requested virtual page.
            const auto pageMatrices = shadowPass.grassPageMatrices(shadowClipMatrices);
            const std::uint32_t pageCount = static_cast<std::uint32_t>(pageMatrices.size());
            const std::uint32_t clusterCount =
                static_cast<std::uint32_t>(std::max<std::size_t>(1, sceneGpu.grassClusters.size()));
            if (pageCount != 0) {
                grassShadowPageMatricesBuffers[currentFrame].update(pageMatrices.data(), sizeof(Mat4) * pageCount);
                const GrassShadowPageCullUniformData pageCullData{pageCount, clusterCount, 0, 0};
                grassShadowPageCullUniformBuffers[currentFrame].update(&pageCullData, sizeof(pageCullData));
                vkCmdFillBuffer(commandBuffer, grassShadowPageDrawCountBuffers[currentFrame].handle(), 0,
                                sizeof(std::uint32_t) * pageCount, 0);
                const VkMemoryBarrier2 pageCullPrepare{
                    VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                    nullptr,
                    VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                    VK_ACCESS_2_TRANSFER_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                    VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT};
                const VkDependencyInfo pageCullPrepareInfo{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                                                           .memoryBarrierCount = 1,
                                                           .pMemoryBarriers = &pageCullPrepare};
                vkCmdPipelineBarrier2(commandBuffer, &pageCullPrepareInfo);
                vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, grassShadowPageCullPipeline);
                const VkDescriptorSet pageCullSet = grassShadowPageCullSets[currentFrame];
                vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                                        grassShadowPageCullPipelineLayout, 0, 1, &pageCullSet, 0, nullptr);
                vkCmdDispatch(commandBuffer, (clusterCount + 63U) / 64U, pageCount, 1);
                const VkMemoryBarrier2 pageCullReady{
                    VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,     nullptr,
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT,  VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT};
                const VkDependencyInfo pageCullReadyInfo{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                                                         .memoryBarrierCount = 1,
                                                         .pMemoryBarriers = &pageCullReady};
                vkCmdPipelineBarrier2(commandBuffer, &pageCullReadyInfo);
            }
            grassShadowDraw.create(grassShadowPageIndirectBuffers[currentFrame].handle(),
                                   grassShadowPageDrawCountBuffers[currentFrame].handle(), clusterCount);
            grassShadowDrawPtr = &grassShadowDraw;
            shadowPass.setGrassShadowVisibleInstances(currentFrame, lists.drawInstances[1].handle());
            sceneDescriptorPass.setGrassShadowVisibleInstances(currentFrame, lists.drawInstances[1].handle());
        }

        // The forward descriptor layout always contains the shadow-map
        // sampler. Even when shadows are disabled, run an empty shadow
        // pass so its image is transitioned from UNDEFINED to
        // SHADER_READ_ONLY_OPTIMAL before the descriptor is used.
        // Mark VSM pages from the completed depth hierarchy. The result
        // is consumed the next time this frame slot is reused, so this
        // dispatch never creates a CPU/GPU synchronization point.
        const auto recordVsmPageRequests = [&](const VkCommandBuffer buffer, const bool sceneView) {
            VsmFeedbackKey feedbackKey = sceneView ? sceneVsmFeedbackKey : hiZFeedbackKeys[previousHiZFrame];
            for (std::uint32_t level = 0; level < ShadowMap::ClipLevelCount; ++level)
                feedbackKey.clipMatrices[level] =
                    (sceneView ? sceneShadowClipMatrices : shadowClipMatrices)[level].native();
            feedbackKey.settingsRevision = renderSettingsRevision;
            feedbackKey.quality = static_cast<std::uint32_t>(shadowQuality);
            const bool needsFeedback = sceneView
                                           ? sceneVsmFeedbackCache.needsUpdate(feedbackKey)
                                           : (!vsmRequestsReady[currentFrame] || vsmRequestsFromScene[currentFrame] ||
                                              vsmFeedbackCaches[currentFrame].needsUpdate(feedbackKey));
            if (mainLightShadows && (sceneView || (renderGameViewport && hadPreviousHiZ)) && needsFeedback &&
                vsmPageMarkingPipeline != VK_NULL_HANDLE && vsmPageMarkingSets[currentFrame] != VK_NULL_HANDLE &&
                vsmPageCompactPipeline != VK_NULL_HANDLE && vsmPageCompactSets[currentFrame] != VK_NULL_HANDLE) {
                vkCmdFillBuffer(buffer, vsmRequestedPageBuffers[currentFrame].handle(), 0, VK_WHOLE_SIZE, 0);
                const VkBufferMemoryBarrier2 clearBarrier{.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
                                                          .srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                                                          .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                                          .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                                          .dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
                                                                           VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                                                          .buffer = vsmRequestedPageBuffers[currentFrame].handle(),
                                                          .offset = 0,
                                                          .size = VK_WHOLE_SIZE};
                const VkDependencyInfo clearDependency{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                                                       .bufferMemoryBarrierCount = 1,
                                                       .pBufferMemoryBarriers = &clearBarrier};
                vkCmdPipelineBarrier2(buffer, &clearDependency);
                gpuTimestampProfiler.beginZone(buffer, currentFrame, shadowPageMarkProfileName);
                vkCmdBindPipeline(buffer, VK_PIPELINE_BIND_POINT_COMPUTE, vsmPageMarkingPipeline);
                const VkDescriptorSet markingSet = vsmPageMarkingSets[currentFrame];
                vkCmdBindDescriptorSets(buffer, VK_PIPELINE_BIND_POINT_COMPUTE, vsmPageMarkingPipelineLayout, 0, 1,
                                        &markingSet, 0, nullptr);
                constexpr std::uint32_t feedbackStride = 2;
                const std::uint32_t feedbackWidth = (feedbackKey.width + feedbackStride - 1U) / feedbackStride;
                const std::uint32_t feedbackHeight = (feedbackKey.height + feedbackStride - 1U) / feedbackStride;
                vkCmdDispatch(buffer, (feedbackWidth + 15U) / 16U, (feedbackHeight + 15U) / 16U, 1);
                const VkBufferMemoryBarrier2 markingBarrier{.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
                                                            .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                                            .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                                                            .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                                            .dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
                                                            .buffer = vsmRequestedPageBuffers[currentFrame].handle(),
                                                            .offset = 0,
                                                            .size = VK_WHOLE_SIZE};
                const VkDependencyInfo markingDependency{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                                                         .bufferMemoryBarrierCount = 1,
                                                         .pBufferMemoryBarriers = &markingBarrier};
                vkCmdPipelineBarrier2(buffer, &markingDependency);
                gpuTimestampProfiler.endZone(buffer, currentFrame);
                gpuTimestampProfiler.beginZone(buffer, currentFrame, shadowPageCompactProfileName);
                vkCmdFillBuffer(buffer, vsmCompactedPageCountBuffers[currentFrame].handle(), 0, sizeof(std::uint32_t),
                                0);
                const VkBufferMemoryBarrier2 countBarrier{.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
                                                          .srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                                                          .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                                          .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                                          .dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
                                                                           VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                                                          .buffer = vsmCompactedPageCountBuffers[currentFrame].handle(),
                                                          .offset = 0,
                                                          .size = sizeof(std::uint32_t)};
                const VkDependencyInfo countDependency{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                                                       .bufferMemoryBarrierCount = 1,
                                                       .pBufferMemoryBarriers = &countBarrier};
                vkCmdPipelineBarrier2(buffer, &countDependency);
                vkCmdBindPipeline(buffer, VK_PIPELINE_BIND_POINT_COMPUTE, vsmPageCompactPipeline);
                const VkDescriptorSet compactSet = vsmPageCompactSets[currentFrame];
                vkCmdBindDescriptorSets(buffer, VK_PIPELINE_BIND_POINT_COMPUTE, vsmPageCompactPipelineLayout, 0, 1,
                                        &compactSet, 0, nullptr);
                constexpr std::uint32_t vsmRequestWordCount = (ShadowMap::VirtualPageCount + 31U) / 32U;
                constexpr std::uint32_t vsmCompactionThreadsPerGroup = 64U;
                vkCmdDispatch(buffer,
                              (vsmRequestWordCount + vsmCompactionThreadsPerGroup - 1U) / vsmCompactionThreadsPerGroup,
                              1, 1);
                const VkBufferMemoryBarrier2 completionBarriers[] = {
                    {.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
                     .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                     .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                     .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT,
                     .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT,
                     .buffer = vsmCompactedPageBuffers[currentFrame].handle(),
                     .offset = 0,
                     .size = VK_WHOLE_SIZE},
                    {.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
                     .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                     .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                     .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT,
                     .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT,
                     .buffer = vsmCompactedPageCountBuffers[currentFrame].handle(),
                     .offset = 0,
                     .size = sizeof(std::uint32_t)}};
                const VkDependencyInfo completionDependency{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                                                            .bufferMemoryBarrierCount = std::size(completionBarriers),
                                                            .pBufferMemoryBarriers = completionBarriers};
                vkCmdPipelineBarrier2(buffer, &completionDependency);
                vsmRequestsReady[currentFrame] = true;
                vsmRequestsFromScene[currentFrame] = sceneView;
                vsmRequestClipMatrices[currentFrame] = sceneView ? sceneShadowClipMatrices : shadowClipMatrices;
                if (sceneView) {
                    sceneVsmFeedbackPending[currentFrame] = true;
                    sceneVsmFeedbackCache.commit(feedbackKey);
                } else
                    vsmFeedbackCaches[currentFrame].commit(feedbackKey);
                gpuTimestampProfiler.endZone(buffer, currentFrame);
            }
        };
        // Scene View lighting reads its own resolved visibility. Update the
        // set before anything in this frame binds it.
        if (renderSceneViewport && sceneDirectionalVisibilityPass.resultView(currentFrame) != VK_NULL_HANDLE)
            sceneDescriptorPass.setDirectionalVisibility(
                currentFrame, {sceneDirectionalVisibilityPass.resultSampler(currentFrame),
                               sceneDirectionalVisibilityPass.resultView(currentFrame),
                               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
        if (renderGameViewport) {
            // Update the shared scene descriptor before the depth prepass
            // binds it; the VSM pass records later in the frame graph.
            const VkDescriptorImageInfo gtaoDebugTexture{gtaoPass.debugSampler(gtaoDebugView),
                                                         gtaoPass.debugView(gtaoDebugView),
                                                         gtaoPass.debugLayout(gtaoDebugView)};
            shadowPass.setGtaoTexture(currentFrame, gtaoDebugTexture);
            if (!msaa.enabled() && directionalVisibilityPass.resultView(currentFrame) != VK_NULL_HANDLE) {
                const VkDescriptorImageInfo directionalTexture{directionalVisibilityPass.resultSampler(currentFrame),
                                                               directionalVisibilityPass.resultView(currentFrame),
                                                               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
                shadowPass.setDirectionalVisibility(currentFrame, directionalTexture);
            }
            if (vulkanDevice.supportsRayQuery() && !msaa.enabled() &&
                rtContactShadowSettings.mode == ContactShadowMode::RayTraced &&
                rtContactShadowPass.resultView(currentFrame) != VK_NULL_HANDLE) {
                shadowPass.setContactShadowTexture(currentFrame, {rtContactShadowPass.resultSampler(currentFrame),
                                                                  rtContactShadowPass.resultView(currentFrame),
                                                                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
            }
            shadowPass.setShadowInstanceTransforms(currentFrame, shadowInstanceTransformBuffers[currentFrame].handle(),
                                                   shadowInstanceMaterialBuffers[currentFrame].handle());
            shadowPass.setShadowInstanceTransforms(currentFrame,
                                                   shadowTwoSidedInstanceTransformBuffers[currentFrame].handle(),
                                                   shadowTwoSidedInstanceMaterialBuffers[currentFrame].handle(), true);
        }

        // Scene View has its own descriptor pass and therefore its own
        // shadow-map image. It must be transitioned as well, even when
        // shadows are disabled, because the forward fragment shader still
        // samples the shadow binding declared by the shared pipeline.
        if (renderSceneViewport) {
            gpuTimestampProfiler.beginZone(commandBuffer, currentFrame, shadowProfileName);
            gpuTimestampProfiler.beginZone(commandBuffer, currentFrame, shadowDepthRasterProfileName);
            // The Scene View owns a distinct VSM page table. Rebuild the
            // shared scratch ranges only after Game View has consumed its
            // own ranges above.
            if (!sceneGpu.grassInstances.empty()) {
                const auto pageMatrices = sceneDescriptorPass.grassPageMatrices(sceneShadowClipMatrices);
                const std::uint32_t pageCount = static_cast<std::uint32_t>(pageMatrices.size());
                const std::uint32_t clusterCount =
                    static_cast<std::uint32_t>(std::max<std::size_t>(1, sceneGpu.grassClusters.size()));
                if (pageCount != 0) {
                    grassShadowPageMatricesBuffers[currentFrame].update(pageMatrices.data(), sizeof(Mat4) * pageCount);
                    const GrassShadowPageCullUniformData pageCullData{pageCount, clusterCount, 0, 0};
                    grassShadowPageCullUniformBuffers[currentFrame].update(&pageCullData, sizeof(pageCullData));
                    vkCmdFillBuffer(commandBuffer, grassShadowPageDrawCountBuffers[currentFrame].handle(), 0,
                                    sizeof(std::uint32_t) * pageCount, 0);
                    const VkMemoryBarrier2 prepare{
                        VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                        nullptr,
                        VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                        VK_ACCESS_2_TRANSFER_WRITE_BIT,
                        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                        VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT};
                    const VkDependencyInfo prepareInfo{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                                                       .memoryBarrierCount = 1,
                                                       .pMemoryBarriers = &prepare};
                    vkCmdPipelineBarrier2(commandBuffer, &prepareInfo);
                    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, grassShadowPageCullPipeline);
                    const VkDescriptorSet pageCullSet = grassShadowPageCullSets[currentFrame];
                    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                                            grassShadowPageCullPipelineLayout, 0, 1, &pageCullSet, 0, nullptr);
                    vkCmdDispatch(commandBuffer, (clusterCount + 63U) / 64U, pageCount, 1);
                    const VkMemoryBarrier2 ready{
                        VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,     nullptr,
                        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                        VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT,  VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT};
                    const VkDependencyInfo readyInfo{
                        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &ready};
                    vkCmdPipelineBarrier2(commandBuffer, &readyInfo);
                }
            }
            sceneDescriptorPass.setShadowInstanceTransforms(currentFrame,
                                                            shadowInstanceTransformBuffers[currentFrame].handle(),
                                                            shadowInstanceMaterialBuffers[currentFrame].handle());
            sceneDescriptorPass.setShadowInstanceTransforms(
                currentFrame, shadowTwoSidedInstanceTransformBuffers[currentFrame].handle(),
                shadowTwoSidedInstanceMaterialBuffers[currentFrame].handle(), true);
            const ShadowPass::MeshletStream meshletShadowStream = shadowMeshletStream(currentFrame);
            const ShadowPass::MeshletStream twoSidedMeshletShadowStream = shadowMeshletStream(currentFrame, true);
            sceneDescriptorPass.record(
                commandBuffer, sceneShadowClipMatrices, sceneShadowClipUpdateMask, vertexBuffer.handle(),
                instanceBuffers[currentFrame].handle(), indexBuffer.handle(),
                sceneDescriptorPass.shadowDescriptorSet(currentFrame),
                sceneDescriptorPass.shadowTwoSidedDescriptorSet(currentFrame), shadowCullingPasses[currentFrame],
                shadowIndirectDraws[currentFrame], shadowTwoSidedCullingPasses[currentFrame],
                shadowTwoSidedIndirectDraws[currentFrame],
                mainLightShadows ? static_cast<std::uint32_t>(gpuObjects.size()) : 0u,
                sceneDescriptorPass.grassShadowDescriptorSet(currentFrame),
                mainLightShadows ? grassShadowDrawPtr : nullptr,
                static_cast<std::uint32_t>(std::max<std::size_t>(1, sceneGpu.grassClusters.size())),
                &meshletShadowStream, &twoSidedMeshletShadowStream);
            gpuTimestampProfiler.endZone(commandBuffer, currentFrame);
            gpuTimestampProfiler.endZone(commandBuffer, currentFrame);
        }
        // Forward and Scene View sample the masks every frame; give a slot
        // that has never been traced a defined layout and "no receiver".
        if (!localShadowMaskInitialized[currentFrame]) {
            localLightShadowPass.clear(commandBuffer, currentFrame);
            localShadowMaskInitialized[currentFrame] = true;
        }
        gpuTimestampProfiler.beginZone(commandBuffer, currentFrame, cullingProfileName);
        // Closed by the Game View prepass, or before Scene View when that is
        // the only view; it otherwise spanned the whole Scene View frame.
        bool cullingZoneOpen = true;
        const auto dispatchClusteredLights = [&](VkDescriptorSet set, const VkExtent2D extent) {
            const uint32_t tilesX = (extent.width + ClusterTileSize - 1U) / ClusterTileSize;
            const uint32_t tilesY = (extent.height + ClusterTileSize - 1U) / ClusterTileSize;
            vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, clusteredLightingPipeline);
            vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, clusteredLightingPipelineLayout, 0,
                                    1, &set, 0, nullptr);
            vkCmdDispatch(commandBuffer, (tilesX + 3U) / 4U, (tilesY + 3U) / 4U, (ClusterDepthSlices + 3U) / 4U);
            const VkMemoryBarrier2 barrier{
                VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,      nullptr,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,  VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT};
            const VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO, nullptr, 0, 1, &barrier};
            vkCmdPipelineBarrier2(commandBuffer, &dependency);
        };
        static const ProfileNameId clusteredLightsProfileName = Profiler::registerName("Culling.Clustered Lights");
        static const ProfileNameId objectCullingProfileName = Profiler::registerName("Culling.Objects");
        gpuTimestampProfiler.beginZone(commandBuffer, currentFrame, clusteredLightsProfileName);
        if (renderGameViewport)
            dispatchClusteredLights(clusteredLightingSets[currentFrame], swapchain.extent());
        if (renderSceneViewport)
            dispatchClusteredLights(sceneClusteredLightingSets[currentFrame], sceneViewportTarget.extent());
        gpuTimestampProfiler.endZone(commandBuffer, currentFrame);
        // hasMaterialPipeline() probes the shader-graph pipeline registry, and
        // a large scene holds tens of thousands of objects spread over a
        // handful of shader slots. Collect the slots that are actually
        // present first, then resolve each of them at most once.
        std::bitset<MaterialProgramSlotCount> presentShaderSlots;
        for (const Culling::GPUObjectData& object : gpuObjects) {
            if (object.shader < MaterialProgramSlotCount)
                presentShaderSlots.set(object.shader);
        }
        std::bitset<MaterialProgramSlotCount> activeShaderSlots;
        for (std::size_t slot = 0; slot < MaterialProgramSlotCount; ++slot) {
            if (presentShaderSlots.test(slot) && forwardPass.hasMaterialPipeline(static_cast<std::uint32_t>(slot)))
                activeShaderSlots.set(slot);
        }
        // Step 1 of the frame migration: culling now has an explicit
        // graph node and declared indirect outputs.  It still executes in
        // this command buffer, preserving all existing ordering while the
        // following raster passes are migrated.
        earlyFrameGraph.reset();
        earlyFrameGraph.enablePassCulling();
        earlyFrameGraph.setQueueFamily(RenderGraph::Queue::Graphics, vulkanDevice.graphicsQueueFamily());
        const RenderGraph::BufferDesc indirectDesc{.size = indirectBuffers[currentFrame].size(),
                                                   .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                                            VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT};
        const RenderGraph::BufferDesc drawCountDesc{.size = drawCountBuffers[currentFrame].size(),
                                                    .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                                             VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT};
        const auto graphIndirect =
            earlyFrameGraph.importBuffer("Main indirect draws", indirectBuffers[currentFrame].handle(), indirectDesc);
        const auto graphDrawCount =
            earlyFrameGraph.importBuffer("Main draw counts", drawCountBuffers[currentFrame].handle(), drawCountDesc);
        earlyFrameGraph.addPass(
            "GPU culling", RenderGraph::Queue::Graphics,
            [&](RenderGraph::PassBuilder& builder) {
                builder.write(graphIndirect, RenderGraph::BufferUsage::StorageWriteCompute);
                builder.write(graphDrawCount, RenderGraph::BufferUsage::StorageWriteCompute);
            },
            [&](const VkCommandBuffer buffer) {
                gpuCullingPasses[currentFrame].recordBinned(buffer, static_cast<std::uint32_t>(gpuObjects.size()),
                                                            MaterialProgramSlotCount);
            });
        earlyFrameGraph.exportBuffer(graphIndirect);
        earlyFrameGraph.exportBuffer(graphDrawCount);
        gpuTimestampProfiler.beginZone(commandBuffer, currentFrame, objectCullingProfileName);
        earlyFrameGraph.execute(commandBuffer);
        gpuTimestampProfiler.endZone(commandBuffer, currentFrame);
        profileRenderGraph(earlyFrameGraph, false);
        if (renderGameViewport && virtualWaterRenderer.active()) {
            static const ProfileNameId waterPageCullProfileName = Profiler::registerName("Water Page Cull");
            gpuTimestampProfiler.beginZone(commandBuffer, currentFrame, waterPageCullProfileName);
            virtualWaterRenderer.recordCull(commandBuffer, currentFrame, shadowPass.descriptorSet(currentFrame),
                                            hadPreviousHiZ);
            virtualWaterRenderer.recordState(commandBuffer, currentFrame, shadowPass.descriptorSet(currentFrame),
                                             static_cast<float>(Time::deltaTime()));
            gpuTimestampProfiler.endZone(commandBuffer, currentFrame);
        }
        // The Scene View cull binds the viewport depth as its (disabled) Hi-Z
        // source; skip it until that depth image has left UNDEFINED. The
        // prepass then draws the zeroed indirect commands for one frame.
        if (renderSceneViewport && sceneViewportDepthInitialized && sceneVirtualWaterRenderer.active()) {
            static const ProfileNameId sceneWaterPageCullProfileName = Profiler::registerName("Scene Water Page Cull");
            gpuTimestampProfiler.beginZone(commandBuffer, currentFrame, sceneWaterPageCullProfileName);
            sceneVirtualWaterRenderer.recordCull(commandBuffer, currentFrame,
                                                 sceneDescriptorPass.descriptorSet(currentFrame), false);
            sceneVirtualWaterRenderer.recordState(commandBuffer, currentFrame,
                                                  sceneDescriptorPass.descriptorSet(currentFrame),
                                                  static_cast<float>(Time::deltaTime()));
            gpuTimestampProfiler.endZone(commandBuffer, currentFrame);
        }
        // The generic instance compaction result is not consumed by the
        // active draw path. Do not dispatch it until it directly feeds
        // instance-driven commands; cluster culling remains the live
        // grass path.
        constexpr bool enableExperimentalGpuSceneGrassCompaction = false;
        // GPU grass: classify visible GPU-scene instances, prefix compact
        // ranges, scatter renderer indices, then emit indirect commands.
        const std::uint32_t grassBinCount =
            std::max(1u, static_cast<std::uint32_t>(sceneGpu.database.meshes().size() * 3u));
        const std::uint32_t visibleCapacity = static_cast<std::uint32_t>(sceneGpu.database.instances().size());
        if constexpr (enableExperimentalGpuSceneGrassCompaction) {
            if (visibleCapacity != 0 && cameraController.camera()) {
                const Vec3 cameraPosition = cameraController.camera()->position();
                const GrassIndirectUniformData indirectData{visibleCapacity, visibleCapacity, visibleCapacity,
                                                            grassBinCount, glm::vec4{cameraPosition.native(), 1.0F}};
                const GrassPrefixUniformData prefixData{grassBinCount, visibleCapacity, 0u, 0u};
                grassIndirectUniformBuffers[currentFrame].update(&indirectData, sizeof(indirectData));
                grassPrefixUniformBuffers[currentFrame].update(&prefixData, sizeof(prefixData));
                vkCmdFillBuffer(commandBuffer, grassBinCountBuffers[currentFrame].handle(), 0, VK_WHOLE_SIZE, 0);
                vkCmdFillBuffer(commandBuffer, grassBinCursorBuffers[currentFrame].handle(), 0, VK_WHOLE_SIZE, 0);
                vkCmdFillBuffer(commandBuffer, grassDrawCountBuffers[currentFrame].handle(), 0, sizeof(std::uint32_t),
                                0);
                vkCmdFillBuffer(commandBuffer, grassIndirectBuffers[currentFrame].handle(), 0, VK_WHOLE_SIZE, 0);
                const VkMemoryBarrier2 clearBarrier{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                                                    .srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                                                    .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                                    .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                                    .dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
                                                                     VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT};
                const VkDependencyInfo clearDependency{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                                                       .memoryBarrierCount = 1,
                                                       .pMemoryBarriers = &clearBarrier};
                vkCmdPipelineBarrier2(commandBuffer, &clearDependency);
                const auto dispatch = [&](const VkPipeline pipeline, const VkPipelineLayout layout,
                                          const VkDescriptorSet set, const std::uint32_t groups) {
                    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
                    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0,
                                            nullptr);
                    vkCmdDispatch(commandBuffer, groups, 1, 1);
                    const VkMemoryBarrier2 barrier{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                                                   .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                                   .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                                                   .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                                   .dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
                                                                    VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT};
                    const VkDependencyInfo dependency{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                                                      .memoryBarrierCount = 1,
                                                      .pMemoryBarriers = &barrier};
                    vkCmdPipelineBarrier2(commandBuffer, &dependency);
                };
                dispatch(grassBuildPipeline, grassBuildPipelineLayout, grassBuildSets[currentFrame],
                         (visibleCapacity + 63u) / 64u);
                dispatch(grassPrefixPipeline, grassPrefixPipelineLayout, grassPrefixSets[currentFrame], 1u);
                dispatch(grassScatterPipeline, grassScatterPipelineLayout, grassScatterSets[currentFrame],
                         (visibleCapacity + 63u) / 64u);
                dispatch(grassFinalizePipeline, grassFinalizePipelineLayout, grassFinalizeSets[currentFrame],
                         (grassBinCount + 63u) / 64u);
                const VkMemoryBarrier2 grassDrawBarrier{
                    .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                    .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                    .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                    .dstStageMask = VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
                    .dstAccessMask = VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT};
                const VkDependencyInfo grassDrawDependency{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                                                           .memoryBarrierCount = 1,
                                                           .pMemoryBarriers = &grassDrawBarrier};
                vkCmdPipelineBarrier2(commandBuffer, &grassDrawDependency);
            }
        }
        // Scene View does not run the GTAO graph. Bootstrap its white AO
        // texture here only when it is the active viewport.
        if (!renderGameViewport && !gtaoPass.resultInitialized())
            gtaoPass.initialize(commandBuffer);
        // Forward always binds the SSGI result, so it must be black and
        // sampleable before any view draws. A traced frame bootstraps it on
        // the trace's own queue instead, as GTAO does.
        if (!ssgiTracedThisFrame && !ssgiPass.initialized())
            ssgiPass.initialize(commandBuffer);
        if (!ddgiTracedThisFrame && ddgiPass.available() && !ddgiPass.initialized())
            ddgiPass.initialize(commandBuffer);
        if (renderGameViewport) {
            viewportFrameGraph.reset();
            viewportFrameGraph.enablePassCulling();
            viewportFrameGraph.setQueueFamily(RenderGraph::Queue::Graphics, vulkanDevice.graphicsQueueFamily());
            viewportFrameGraph.setQueueFamily(RenderGraph::Queue::AsyncCompute,
                                              vulkanDevice.hasAsyncComputeQueue() ? vulkanDevice.computeQueueFamily()
                                                                                  : vulkanDevice.graphicsQueueFamily());
            viewportFrameGraph.setQueueFamily(RenderGraph::Queue::Transfer, renderPath == RenderPath::AsyncCompute &&
                                                                                    vulkanDevice.hasAsyncTransferQueue()
                                                                                ? vulkanDevice.transferQueueFamily()
                                                                                : vulkanDevice.graphicsQueueFamily());
            const VkExtent2D graphExtent = swapchain.extent();
            const bool sharedDepthReads = renderPath == RenderPath::AsyncCompute &&
                                          vulkanDevice.hasAsyncComputeQueue() &&
                                          vulkanDevice.computeQueueFamily() != vulkanDevice.graphicsQueueFamily();
            const RenderGraph::TextureDesc graphDepthDesc{
                .extent = {graphExtent.width, graphExtent.height, 1},
                .format = msaa.enabled() ? hiZDepthBuffer.format() : depthBuffer.format(),
                .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                .aspect = VK_IMAGE_ASPECT_DEPTH_BIT,
                .concurrentSharing = sharedDepthReads};
            const RenderGraph::TextureDesc graphHdrDesc{
                .extent = {graphExtent.width, graphExtent.height, 1},
                .format = HdrBuffer::Format,
                .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                         VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                .aspect = VK_IMAGE_ASPECT_COLOR_BIT};
            const RenderGraph::TextureState graphHdrInitialState =
                hdrBufferInitialized ? RenderGraph::TextureState{.stage = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                                                                 .access = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                                                                 .layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}
                                     : RenderGraph::TextureState{.stage = VK_PIPELINE_STAGE_2_NONE,
                                                                 .access = VK_ACCESS_2_NONE,
                                                                 .layout = VK_IMAGE_LAYOUT_UNDEFINED};
            const auto graphPrepassHdr =
                viewportFrameGraph.importTexture("Forward HDR", hdrBuffer.image(), graphHdrDesc, graphHdrInitialState);
            // The prepass clears this image, so the graph may discard old contents.
            const auto graphDepth = viewportFrameGraph.importTexture(
                "Forward depth", msaa.enabled() ? hiZDepthBuffer.image() : depthBuffer.image(), graphDepthDesc,
                {.layout = VK_IMAGE_LAYOUT_UNDEFINED});
            const RenderGraph::TextureDesc graphVelocityDesc{.extent = {graphExtent.width, graphExtent.height, 1},
                                                             .format = VK_FORMAT_R16G16_SFLOAT,
                                                             .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                                                      VK_IMAGE_USAGE_SAMPLED_BIT,
                                                             .aspect = VK_IMAGE_ASPECT_COLOR_BIT};
            const RenderGraph::TextureDesc graphViewNormalDesc{.extent = {graphExtent.width, graphExtent.height, 1},
                                                               .format = VK_FORMAT_R16G16_SNORM,
                                                               .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                                                        VK_IMAGE_USAGE_SAMPLED_BIT,
                                                               .aspect = VK_IMAGE_ASPECT_COLOR_BIT,
                                                               .concurrentSharing = sharedDepthReads};
            const auto graphVelocity =
                !msaa.enabled()
                    ? viewportFrameGraph.importTexture("Forward velocity", velocityBuffer.image(), graphVelocityDesc,
                                                       {.layout = VK_IMAGE_LAYOUT_UNDEFINED})
                    : RenderGraph::TextureHandle{};
            const auto graphViewNormal =
                !msaa.enabled()
                    ? viewportFrameGraph.importTexture("GTAO view normals", gtaoViewNormalBuffer.image(),
                                                       graphViewNormalDesc, {.layout = VK_IMAGE_LAYOUT_UNDEFINED})
                    : RenderGraph::TextureHandle{};
            const RenderGraph::BufferDesc graphVertexDesc{.size = vertexBuffer.size(),
                                                          .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT};
            const RenderGraph::BufferDesc graphIndexDesc{.size = indexBuffer.size(),
                                                         .usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT};
            const auto graphVertices =
                viewportFrameGraph.importBuffer("Geometry vertices", vertexBuffer.handle(), graphVertexDesc);
            const auto graphIndices =
                viewportFrameGraph.importBuffer("Geometry indices", indexBuffer.handle(), graphIndexDesc);
            if (vertexBuffer.readyTimeline() != 0)
                viewportFrameGraph.markUploaded(graphVertices, vertexBuffer.readyTimeline());
            if (indexBuffer.readyTimeline() != 0)
                viewportFrameGraph.markUploaded(graphIndices, indexBuffer.readyTimeline());
            const RenderGraph::BufferDesc foliageIndirectDesc{.size = foliageIndirectBuffers[currentFrame].size(),
                                                              .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                                                       VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT};
            const RenderGraph::BufferDesc foliageDrawCountDesc{.size = foliageDrawCountBuffers[currentFrame].size(),
                                                               .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                                                        VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT};
            const auto graphFoliageIndirect = viewportFrameGraph.importBuffer(
                "Foliage indirect draws", foliageIndirectBuffers[currentFrame].handle(), foliageIndirectDesc);
            const auto graphFoliageDrawCount = viewportFrameGraph.importBuffer(
                "Foliage draw counts", foliageDrawCountBuffers[currentFrame].handle(), foliageDrawCountDesc);
            const RenderGraph::TextureDesc graphHiZDesc{.extent = {hiZBuffer.width(), hiZBuffer.height(), 1},
                                                        .format = VK_FORMAT_R32_SFLOAT,
                                                        .usage =
                                                            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                                                        .aspect = VK_IMAGE_ASPECT_COLOR_BIT,
                                                        .mipLevels = hiZBuffer.mipCount(),
                                                        .concurrentSharing = hiZBuffer.concurrentSharing()};
            const auto graphHiZ =
                hizEnabled ? viewportFrameGraph.importTexture("Hi-Z pyramid", hiZBuffer.image(), graphHiZDesc,
                                                              {.stage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                                               .access = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                                                               .layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL})
                           : RenderGraph::TextureHandle{};
            const auto graphPreviousHiZ =
                hizEnabled && hadPreviousHiZ
                    ? viewportFrameGraph.importTexture("Previous Hi-Z pyramid", previousHiZBuffer.image(), graphHiZDesc,
                                                       {.stage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                                        .access = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                                                        .layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL})
                    : RenderGraph::TextureHandle{};
            const auto aoExtent = gtaoPass.resultExtent();
            const RenderGraph::TextureDesc graphAoDesc{.extent = {aoExtent.width, aoExtent.height, 1},
                                                       .format = gtaoPass.resultFormat(),
                                                       .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                                                       .aspect = VK_IMAGE_ASPECT_COLOR_BIT,
                                                       .concurrentSharing = vulkanDevice.hasAsyncComputeQueue() &&
                                                                            vulkanDevice.computeQueueFamily() !=
                                                                                vulkanDevice.graphicsQueueFamily()};
            const RenderGraph::TextureState graphAoInitialState =
                gtaoPass.resultInitialized()
                    ? RenderGraph::TextureState{.stage = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                                                .access = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                                                .layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}
                    : RenderGraph::TextureState{.layout = VK_IMAGE_LAYOUT_UNDEFINED};
            const auto graphAo = viewportFrameGraph.importTexture("GTAO visibility", gtaoPass.resultImage(),
                                                                  graphAoDesc, graphAoInitialState);
            viewportFrameGraph.addPass(
                "Foliage GPU culling", RenderGraph::Queue::Graphics,
                [&](RenderGraph::PassBuilder& builder) {
                    builder.write(graphFoliageIndirect, RenderGraph::BufferUsage::StorageWriteCompute);
                    builder.write(graphFoliageDrawCount, RenderGraph::BufferUsage::StorageWriteCompute);
                },
                [&](const VkCommandBuffer buffer) {
                    foliageGpuCullingPasses[currentFrame].recordBinned(
                        buffer, static_cast<std::uint32_t>(gpuObjects.size()), MaterialProgramSlotCount);
                });
            viewportFrameGraph.addPass(
                "Depth / prepass", RenderGraph::Queue::Graphics,
                [&](RenderGraph::PassBuilder& builder) {
                    builder.write(graphPrepassHdr, RenderGraph::TextureUsage::ColorAttachment);
                    builder.write(graphDepth, RenderGraph::TextureUsage::DepthAttachment);
                    if (graphVelocity)
                        builder.write(graphVelocity, RenderGraph::TextureUsage::ColorAttachment);
                    if (graphViewNormal) {
                        builder.write(graphViewNormal, RenderGraph::TextureUsage::ColorAttachment);
                        builder.setFinalTextureState(graphViewNormal,
                                                     {.stage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                                      .access = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                                                      .layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                                      .write = false});
                    }
                    builder.read(graphVertices, RenderGraph::BufferUsage::VertexRead);
                    builder.read(graphIndices, RenderGraph::BufferUsage::IndexRead);
                    builder.read(graphFoliageIndirect, RenderGraph::BufferUsage::IndirectRead);
                    builder.read(graphFoliageDrawCount, RenderGraph::BufferUsage::IndirectRead);
                    // This prepass is the only writer of TAA velocity. The
                    // resolve lives in the presentation graph, so velocity has
                    // no reader inside this graph and pass culling would drop
                    // the prepass whenever GTAO and visibility are disabled.
                    if (antialiasingLevel == AntialiasingLevel::TAA)
                        builder.setSideEffect();
                    builder.setFinalTextureState(graphDepth, {.stage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                                                                       VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                                                                       VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                                                              .access = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT |
                                                                        VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT,
                                                              .layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
                                                              .write = false});
                },
                [&](const VkCommandBuffer buffer) {
                    if (cullingZoneOpen) {
                        gpuTimestampProfiler.endZone(buffer, currentFrame);
                        cullingZoneOpen = false;
                    }
                    gpuTimestampProfiler.beginZone(buffer, currentFrame, depthPrepassProfileName);
                    forwardPass.begin(buffer, msaa.enabled() ? msaa.colorImageView() : hdrBuffer.imageView(),
                                      depthBuffer.imageView(),
                                      msaa.enabled() ? VK_NULL_HANDLE : velocityBuffer.imageView(),
                                      msaa.enabled() ? VK_NULL_HANDLE : gtaoViewNormalBuffer.imageView(),
                                      msaa.enabled() ? hdrBuffer.imageView() : VK_NULL_HANDLE,
                                      msaa.enabled() ? hiZDepthBuffer.imageView() : VK_NULL_HANDLE, swapchain.extent(),
                                      shadowPass.descriptorSet(currentFrame), vertexBuffer.handle(),
                                      instanceBuffers[currentFrame].handle(), indexBuffer.handle());
                    if (meshletForwardReady)
                        vkCmdBindIndexBuffer(buffer, indexBuffer.handle(), 0, VK_INDEX_TYPE_UINT32);
                    for (std::uint32_t shader = 0; shader < MaterialProgramSlotCount; ++shader) {
                        if (!activeShaderSlots.test(shader) ||
                            isCompositedProgramSlot(shader))
                            continue;
                        const auto countOffset = static_cast<VkDeviceSize>(shader) * sizeof(std::uint32_t);
                        if (meshShaderForward) {
                            forwardPass.drawMeshShaderMaterial(buffer, shadowPass.descriptorSet(currentFrame),
                                                               meshletDrawSets[currentFrame], shader,
                                                               meshletDrawCommandBuffers[currentFrame].handle());
                        } else if (meshletForwardReady) {
                            const auto commandOffset = meshletCommandOffset(shader);
                            forwardPass.drawMeshletMaterial(buffer, shadowPass.descriptorSet(currentFrame), shader,
                                                            meshletIndexedIndirectDraws[currentFrame], commandOffset,
                                                            countOffset);
                        } else {
                            const auto commandOffset = binnedCommandOffset(static_cast<std::uint32_t>(shader), false);
                            const auto twoSidedCommandOffset = binnedCommandOffset(static_cast<std::uint32_t>(shader), true);
                            forwardPass.drawMaterial(buffer, shadowPass.descriptorSet(currentFrame), shader,
                                                     indirectDraws[currentFrame], commandOffset, countOffset);
                            forwardPass.drawMaterial(buffer, shadowPass.descriptorSet(currentFrame), shader,
                                                     foliageIndirectDraws[currentFrame], twoSidedCommandOffset, countOffset,
                                                     true);
                        }
                    }
                    if (meshletForwardReady)
                        vkCmdBindIndexBuffer(buffer, indexBuffer.handle(), 0, VK_INDEX_TYPE_UINT32);
                    if (meshletSkipsFoliage) {
                        for (std::uint32_t shader = 0; shader < MaterialProgramSlotCount; ++shader) {
                            if (!activeShaderSlots.test(shader) || isCompositedProgramSlot(shader))
                                continue;
                            forwardPass.drawMaterial(buffer, shadowPass.descriptorSet(currentFrame), shader,
                                                     foliageIndirectDraws[currentFrame],
                                                     binnedCommandOffset(static_cast<std::uint32_t>(shader), true),
                                                     static_cast<VkDeviceSize>(shader) * sizeof(std::uint32_t), true);
                        }
                    }
                    if (!sceneGpu.grassInstances.empty()) {
                        const auto& lists = grassRenderLists[currentFrame];
                        Culling::IndexedIndirectDrawCount grassDraw;
                        grassDraw.create(
                            lists.mainIndirect.handle(), lists.mainDrawCount.handle(),
                            static_cast<uint32_t>(std::max<std::size_t>(1, sceneGpu.grassClusters.size())));
                        shadowPass.setGrassVisibleInstances(currentFrame, lists.drawInstances[0].handle());
                        forwardPass.drawGrass(buffer, shadowPass.grassDescriptorSet(currentFrame), grassDraw);
                    }
                    ForwardPass::end(buffer);
                    gpuTimestampProfiler.endZone(buffer, currentFrame);
                });
            const auto gtaoQueue = renderPath == RenderPath::AsyncCompute && vulkanDevice.hasAsyncComputeQueue()
                                       ? RenderGraph::Queue::AsyncCompute
                                       : RenderGraph::Queue::Graphics;
            const DepthBuffer& gtaoDepth = msaa.enabled() ? hiZDepthBuffer : depthBuffer;
            const Mat4 inverseProjection{glm::inverse(cameraController.camera()->projectionMatrix().native())};
            viewportFrameGraph.addPass(
                "GTAO", gtaoQueue,
                [&](RenderGraph::PassBuilder& builder) {
                    builder.read(graphDepth, RenderGraph::TextureUsage::DepthReadCompute);
                    if (graphViewNormal)
                        builder.read(graphViewNormal, RenderGraph::TextureUsage::SampledReadCompute);
                    builder.write(graphAo, RenderGraph::TextureUsage::StorageWriteCompute);
                    builder.setFinalTextureState(graphAo, {.stage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                                           .access = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                                                           .layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                                           .write = false});
                },
                [&](const VkCommandBuffer buffer) {
                    // Timestamps stay on graphics; async compute gets a label only.
                    if (gtaoQueue == RenderGraph::Queue::Graphics)
                        gpuTimestampProfiler.beginZone(buffer, currentFrame, gtaoProfileName);
                    gtaoPass.record(buffer, currentFrame,
                                    taaResolveActive ? static_cast<std::uint32_t>(submittedFrameValue) : 0U,
                                    gtaoDepth.imageView(), gtaoDepth.sampler(),
                                    msaa.enabled() ? gtaoDepth.imageView() : gtaoViewNormalBuffer.imageView(),
                                    msaa.enabled() ? gtaoDepth.sampler() : gtaoViewNormalBuffer.sampler(),
                                    !msaa.enabled(), inverseProjection, {.graphOwnsResultState = true});
                    if (gtaoQueue == RenderGraph::Queue::Graphics)
                        gpuTimestampProfiler.endZone(buffer, currentFrame);
                });
            viewportFrameGraph.exportTexture(graphAo);
            RenderGraph::TextureHandle graphSsgi{};
            if (ssgiTracedThisFrame && graphViewNormal) {
                const VkExtent2D ssgiExtent = ssgiPass.resultExtent();
                const RenderGraph::TextureDesc ssgiDesc{.extent = {ssgiExtent.width, ssgiExtent.height, 1},
                                                        .format = SsgiPass::ResultFormat,
                                                        .usage = VK_IMAGE_USAGE_STORAGE_BIT |
                                                                 VK_IMAGE_USAGE_SAMPLED_BIT,
                                                        .aspect = VK_IMAGE_ASPECT_COLOR_BIT,
                                                        .concurrentSharing = graphAoDesc.concurrentSharing};
                graphSsgi = viewportFrameGraph.importTexture(
                    "SSGI irradiance", ssgiPass.resultImage(), ssgiDesc,
                    ssgiPass.initialized()
                        ? RenderGraph::TextureState{.stage = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                                                    .access = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                                                    .layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}
                        : RenderGraph::TextureState{.layout = VK_IMAGE_LAYOUT_UNDEFINED});
                const SsgiFrameInputs ssgiInputs{.depthView = gtaoDepth.imageView(),
                                                 .depthSampler = gtaoDepth.sampler(),
                                                 .viewNormalView = gtaoViewNormalBuffer.imageView(),
                                                 .viewNormalSampler = gtaoViewNormalBuffer.sampler(),
                                                 .projection = cameraController.camera()->projectionMatrix(),
                                                 .viewToPreviousClip = ssgiViewToPreviousClip};
                // Same inputs as GTAO, so it shares GTAO's queue and overlaps
                // the shadow atlas work on graphics.
                viewportFrameGraph.addPass(
                    "SSGI", gtaoQueue,
                    [&](RenderGraph::PassBuilder& builder) {
                        builder.read(graphDepth, RenderGraph::TextureUsage::DepthReadCompute);
                        builder.read(graphViewNormal, RenderGraph::TextureUsage::SampledReadCompute);
                        builder.write(graphSsgi, RenderGraph::TextureUsage::StorageWriteCompute);
                        builder.setFinalTextureState(graphSsgi, {.stage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                                                 .access = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                                                                 .layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                                                 .write = false});
                    },
                    [&, ssgiInputs](const VkCommandBuffer buffer) {
                        // Timestamps stay on graphics; async compute gets a label only.
                        if (gtaoQueue == RenderGraph::Queue::Graphics)
                            gpuTimestampProfiler.beginZone(buffer, currentFrame, ssgiProfileName);
                        ssgiPass.record(buffer, currentFrame, ssgiInputs, ssgiSettings);
                        if (gtaoQueue == RenderGraph::Queue::Graphics)
                            gpuTimestampProfiler.endZone(buffer, currentFrame);
                    });
                viewportFrameGraph.exportTexture(graphSsgi);
            } else {
                ssgiTracedThisFrame = false;
                if (!ssgiPass.initialized())
                    ssgiPass.initialize(commandBuffer);
            }
            // Mega Geometry ray tracing scene, shared by DDGI and the local
            // light shadows: every meshlet a cluster (CLAS), all opaque
            // sections of a mesh one cluster BLAS, one PTLAS instance per
            // object placement. Built and updated on graphics before either
            // traces it.
            const bool localShadowCasters =
                std::any_of(sceneFrameDataCache.data.lights.begin(),
                            sceneFrameDataCache.data.lights.begin() + sceneFrameDataCache.data.lightCount,
                            [](const LocalLightGPU& light) { return light.parameters.z > 0.5F; });
            const bool clusterShadowsPossible = localLightShadowPass.clusterTracing() && renderGameViewport &&
                                                localShadowCasters && ddgiPass.tracePipeline() != VK_NULL_HANDLE;
            // Trial phases of ShadowTracePhaseFrames alternate the cluster scene
            // and the classic TLAS until both have been timed. The trial
            // waits out the frames after a scene load (streaming, first
            // builds, empty histories), and short phases let both backends
            // time nearly the same views: 48-frame phases right after load
            // once picked the 3x slower backend on Bistro.
            constexpr std::uint32_t ShadowTraceWarmupFrames = 120;
            constexpr std::uint32_t ShadowTracePhaseFrames = 16;
            constexpr std::uint32_t ShadowTraceSettleFrames = 6;
            bool clusterShadowsRequested = false;
            shadowTraceSampled[currentFrame] = false;
            if (clusterShadowsPossible) {
                if (shadowTraceBackend == ShadowTraceBackend::Measuring &&
                    shadowTraceTrialFrame < ShadowTraceWarmupFrames) {
                    // Classic TLAS meanwhile: no cluster scene is built yet.
                    ++shadowTraceTrialFrame;
                } else if (shadowTraceBackend == ShadowTraceBackend::Measuring) {
                    const std::uint32_t trialFrame = shadowTraceTrialFrame - ShadowTraceWarmupFrames;
                    const std::uint32_t phaseFrame = trialFrame % ShadowTracePhaseFrames;
                    clusterShadowsRequested = (trialFrame / ShadowTracePhaseFrames) % 2 == 0;
                    // The first frames of a phase build its structures.
                    shadowTraceSampled[currentFrame] = phaseFrame >= ShadowTraceSettleFrames;
                    ++shadowTraceTrialFrame;
                } else {
                    clusterShadowsRequested = shadowTraceBackend == ShadowTraceBackend::Cluster;
                }
            }
            shadowTraceUsedCluster[currentFrame] = clusterShadowsRequested;
            const bool clusterSceneNeeded = ddgiTracedThisFrame || clusterShadowsRequested;
            const DdgiPass::GeometryBuffers clusterGeometry{meshletBuffer.handle(), meshletVertexBuffer.handle(),
                                                            meshletTriangleBuffer.handle(), vertexBuffer.handle()};
            bool clusterTopologyChanged = false;
            if (clusterSceneNeeded) {
                const ClusterRayTracingScene::GeometrySources& builtSources = clusterScene.geometrySources();
                const bool sourcesChanged = builtSources.meshlets != clusterGeometry.meshlets ||
                                            builtSources.meshletVertices != clusterGeometry.meshletVertices ||
                                            builtSources.meshletTriangles != clusterGeometry.meshletTriangles ||
                                            builtSources.vertices != clusterGeometry.vertices;
                const std::uint64_t topologyRevision = registry.renderTopologyRevision();
                clusterInstances.clear();
                clusterBlases.clear();
                clusterVariants.clear();
                clusterInstanceBounds.clear();
                // Sections of one object share its transform and material
                // table, so they collapse into one instance of the mesh BLAS.
                std::unordered_map<std::uint64_t, std::size_t> placements;
                for (std::size_t batchIndex = 0; batchIndex < instanceBatches.size(); ++batchIndex) {
                    const InstanceBatch& batch = instanceBatches[batchIndex];
                    if (batch.alphaMode != AlphaMode::Opaque || batch.mesh == nullptr || batch.meshletCount == 0)
                        continue;
                    const ClusterRayTracingScene::BlasKey key{batch.mesh, 0, 0};
                    clusterBlases.push_back({key, batch.firstMeshlet, batch.meshletCount});
                    for (const std::size_t renderableIndex : sceneGpu.batchRenderableIndices[batchIndex]) {
                        const RendererInstanceData& source = instanceModels[renderableIndex];
                        // Displaced instances trace their own cluster-template
                        // instantiation, shared by every instance using the
                        // same mesh and materials.
                        const bool displaced = renderables[renderableIndex].displacementBoundsPadding > 1.0e-6F;
                        const auto materialBase = std::bit_cast<std::uint32_t>(source.positionMaterial.w);
                        if (displaced) {
                            const ClusterRayTracingScene::VariantInput variant{key, materialBase};
                            if (std::ranges::find(clusterVariants, variant) == clusterVariants.end())
                                clusterVariants.push_back(variant);
                        }
                        const AABB& bounds = renderables[renderableIndex].worldBounds;
                        const std::array<float, 6> box{bounds.min.x(), bounds.min.y(), bounds.min.z(),
                                                       bounds.max.x(), bounds.max.y(), bounds.max.z()};
                        const glm::quat q{source.rotation.w, source.rotation.x, source.rotation.y, source.rotation.z};
                        glm::mat4 model = glm::mat4_cast(q);
                        model[0] *= source.scaleBase.x;
                        model[1] *= source.scaleBase.y;
                        model[2] *= source.scaleBase.z;
                        ClusterRayTracingScene::InstanceInput input{};
                        input.key = key;
                        input.materialBase = materialBase;
                        input.displaced = displaced;
                        for (std::uint32_t row = 0; row < 3; ++row) {
                            for (std::uint32_t column = 0; column < 3; ++column)
                                input.transform[row * 4 + column] = model[column][row];
                            input.transform[row * 4 + 3] = source.positionMaterial[static_cast<int>(row)];
                        }
                        std::uint64_t placement = std::hash<const void*>{}(batch.mesh) ^ (materialBase * 0x9e3779b9ULL);
                        placement ^= displaced ? 0x51ED270BULL : 0ULL;
                        for (const float value : input.transform)
                            placement = (placement ^ std::hash<float>{}(value)) * 1099511628211ULL;
                        const auto [known, inserted] = placements.try_emplace(placement, clusterInstances.size());
                        if (!inserted) {
                            auto& merged = clusterInstanceBounds[known->second];
                            for (int axis = 0; axis < 3; ++axis) {
                                merged[axis] = std::min(merged[axis], box[axis]);
                                merged[axis + 3] = std::max(merged[axis + 3], box[axis + 3]);
                            }
                            continue;
                        }
                        clusterInstanceBounds.push_back(box);
                        clusterInstances.push_back(input);
                    }
                }
                const bool rebuildGeometry =
                    clusterGeometryDirty || sourcesChanged || !clusterScene.hasVariants(clusterVariants);
                clusterTopologyChanged = rebuildGeometry || topologyRevision != clusterTopologyRevision;
                viewportFrameGraph.addPass(
                    "Cluster RT scene", RenderGraph::Queue::Graphics,
                    [&](RenderGraph::PassBuilder& builder) {
                        builder.read(graphVertices, RenderGraph::BufferUsage::StorageReadCompute);
                        builder.setSideEffect();
                    },
                    [&, rebuildGeometry, topologyRevision](const VkCommandBuffer buffer) {
                        gpuTimestampProfiler.beginZone(buffer, currentFrame, clusterSceneProfileName);
                        // Building clusters needs a bound ray tracing pipeline
                        // that allows them; the DDGI one always exists here.
                        if (rebuildGeometry) {
                            clusterScene.buildGeometry(buffer, ddgiPass.tracePipeline(),
                                                       shadowPass.descriptorSet(currentFrame), clusterBlases,
                                                       clusterVariants,
                                                       {clusterGeometry.meshlets, clusterGeometry.meshletVertices,
                                                        clusterGeometry.meshletTriangles, clusterGeometry.vertices});
                            clusterGeometryDirty = false;
                        }
                        clusterScene.updateInstances(buffer, ddgiPass.tracePipeline(), currentFrame, clusterInstances,
                                                     clusterTopologyChanged);
                        clusterTopologyRevision = topologyRevision;
                        gpuTimestampProfiler.endZone(buffer, currentFrame);
                    });
            }
            RenderGraph::TextureHandle graphDdgiIrradiance{};
            RenderGraph::TextureHandle graphDdgiDistance{};
            if (ddgiTracedThisFrame) {
                // Probe cache invalidation. Lighting, sky or structural
                // changes wake every probe; a moved instance wakes the probes
                // around its old and new bounds.
                const auto& lighting = sceneFrameDataCache.data;
                std::uint64_t lightHash = 1469598103934665603ULL;
                const auto mix = [&lightHash](const void* bytes, std::size_t size) {
                    for (std::size_t i = 0; i < size; ++i)
                        lightHash = (lightHash ^ static_cast<const std::uint8_t*>(bytes)[i]) * 1099511628211ULL;
                };
                const std::array<float, 7> sun{lighting.directionalLight.direction.x(),
                                               lighting.directionalLight.direction.y(),
                                               lighting.directionalLight.direction.z(),
                                               lighting.directionalLight.intensity,
                                               lighting.directionalLight.color.r(),
                                               lighting.directionalLight.color.g(),
                                               lighting.directionalLight.color.b()};
                mix(sun.data(), sizeof(sun));
                mix(&lighting.lightCount, sizeof(lighting.lightCount));
                mix(lighting.lights.data(), sizeof(lighting.lights[0]) * lighting.lightCount);
                const VkImageView environment = imageBasedLighting.descriptors()[0].imageView;
                ddgiWakeBoxes.clear();
                bool wakeAll = clusterTopologyChanged || lightHash != ddgiLightHash || environment != ddgiEnvironmentView ||
                               ddgiPreviousTransforms.size() != clusterInstances.size();
                for (std::size_t i = 0; !wakeAll && i < clusterInstances.size(); ++i) {
                    if (clusterInstances[i].transform == ddgiPreviousTransforms[i])
                        continue;
                    if (ddgiWakeBoxes.size() == DdgiPass::MaxWakeBoxes) {
                        wakeAll = true;
                        break;
                    }
                    const auto& before = ddgiPreviousBounds[i];
                    const auto& after = clusterInstanceBounds[i];
                    ddgiWakeBoxes.push_back({std::min(before[0], after[0]), std::min(before[1], after[1]),
                                             std::min(before[2], after[2]), std::max(before[3], after[3]),
                                             std::max(before[4], after[4]), std::max(before[5], after[5])});
                }
                ddgiPass.wakeProbes(currentFrame, {.all = wakeAll, .boxes = ddgiWakeBoxes});
                ddgiLightHash = lightHash;
                ddgiEnvironmentView = environment;
                ddgiPreviousTransforms.resize(clusterInstances.size());
                for (std::size_t i = 0; i < clusterInstances.size(); ++i)
                    ddgiPreviousTransforms[i] = clusterInstances[i].transform;
                ddgiPreviousBounds = clusterInstanceBounds;
                const auto aoSharing = graphAoDesc.concurrentSharing;
                const auto importAtlas = [&](const char* name, const HdrBuffer& atlas, VkExtent2D extent,
                                             VkFormat format) {
                    return viewportFrameGraph.importTexture(
                        name, atlas.image(),
                        {.extent = {extent.width, extent.height, 1},
                         .format = format,
                         .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                         .aspect = VK_IMAGE_ASPECT_COLOR_BIT,
                         .concurrentSharing = aoSharing},
                        ddgiPass.initialized()
                            ? RenderGraph::TextureState{.stage = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                                                                 VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                                        .access = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                                                        .layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}
                            : RenderGraph::TextureState{.layout = VK_IMAGE_LAYOUT_UNDEFINED});
                };
                graphDdgiIrradiance = importAtlas("DDGI irradiance", ddgiPass.irradianceAtlas(),
                                                  ddgiPass.irradianceExtent(), DdgiPass::IrradianceFormat);
                graphDdgiDistance = importAtlas("DDGI distance", ddgiPass.distanceAtlas(), ddgiPass.distanceExtent(),
                                                DdgiPass::DistanceFormat);
                // Graphics queue: the trace reads the vertex and meshlet heaps,
                // which the graphics queue owns exclusively.
                viewportFrameGraph.addPass(
                    "DDGI", RenderGraph::Queue::Graphics,
                    [&](RenderGraph::PassBuilder& builder) {
                        builder.read(graphVertices, RenderGraph::BufferUsage::StorageReadCompute);
                        for (const auto atlas : {graphDdgiIrradiance, graphDdgiDistance}) {
                            builder.write(atlas, RenderGraph::TextureUsage::StorageWriteCompute);
                            builder.setFinalTextureState(atlas, {.stage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                                                 .access = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                                                                 .layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                                                 .write = false});
                        }
                    },
                    [&](const VkCommandBuffer buffer) {
                        gpuTimestampProfiler.beginZone(buffer, currentFrame, ddgiProfileName);
                        ddgiPass.record(buffer, currentFrame, shadowPass.descriptorSet(currentFrame), clusterScene,
                                        clusterGeometry);
                        gpuTimestampProfiler.endZone(buffer, currentFrame);
                    });
                viewportFrameGraph.exportTexture(graphDdgiIrradiance);
                viewportFrameGraph.exportTexture(graphDdgiDistance);
            }
            // Shadow atlas work follows depth on graphics and overlaps GTAO on compute.
            // The previous Hi-Z is read by page marking before the current Hi-Z write.
            // The table is also the explicit graph dependency between atlas
            // publication and visibility sampling. Sharing a depth input alone
            // does not order these two passes.
            const auto graphShadowPageTable = viewportFrameGraph.importBuffer(
                "VSM page table", shadowPass.pageTableBuffer(currentFrame),
                {.size = sizeof(std::uint32_t) * ShadowMap::VirtualPageCount,
                 .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT});
            viewportFrameGraph.addPass(
                "Shadow VSM", RenderGraph::Queue::Graphics,
                [&](RenderGraph::PassBuilder& builder) {
                    builder.write(graphShadowPageTable, RenderGraph::BufferUsage::TransferWrite);
                    builder.read(graphDepth, RenderGraph::TextureUsage::DepthReadCompute);
                    if (graphPreviousHiZ)
                        builder.read(graphPreviousHiZ, RenderGraph::TextureUsage::SampledReadCompute);
                    builder.setSideEffect();
                },
                [&](const VkCommandBuffer buffer) {
                    gameVsmProfiled[currentFrame] = true;
                    gpuTimestampProfiler.beginZone(buffer, currentFrame, shadowProfileName);
                    recordVsmPageRequests(buffer, false);
                    gpuTimestampProfiler.beginZone(buffer, currentFrame, shadowDepthRasterProfileName);
                    const ShadowPass::MeshletStream meshletShadowStream = shadowMeshletStream(currentFrame);
                    const ShadowPass::MeshletStream twoSidedMeshletShadowStream =
                        shadowMeshletStream(currentFrame, true);
                    shadowPass.setProfiler(&gpuTimestampProfiler, currentFrame);
                    shadowPass.record(
                        buffer, shadowClipMatrices, shadowClipUpdateMask, vertexBuffer.handle(),
                        instanceBuffers[currentFrame].handle(), indexBuffer.handle(),
                        shadowPass.shadowDescriptorSet(currentFrame),
                        shadowPass.shadowTwoSidedDescriptorSet(currentFrame), shadowCullingPasses[currentFrame],
                        shadowIndirectDraws[currentFrame], shadowTwoSidedCullingPasses[currentFrame],
                        shadowTwoSidedIndirectDraws[currentFrame],
                        mainLightShadows ? static_cast<std::uint32_t>(gpuObjects.size()) : 0u,
                        shadowPass.grassShadowDescriptorSet(currentFrame),
                        mainLightShadows ? grassShadowDrawPtr : nullptr,
                        static_cast<std::uint32_t>(std::max<std::size_t>(1, sceneGpu.grassClusters.size())),
                &meshletShadowStream, &twoSidedMeshletShadowStream);
                    shadowPass.setProfiler(nullptr, 0);
                    gpuTimestampProfiler.endZone(buffer, currentFrame);
                    gpuTimestampProfiler.endZone(buffer, currentFrame);
                });
            if (!hizEnabled)
                viewportFrameGraph.exportTexture(graphDepth);
            RenderGraph::TextureHandle graphDirectionalVisibility{};
            if (!msaa.enabled() && directionalVisibilityPass.resultImage(currentFrame) != VK_NULL_HANDLE) {
                const VkExtent2D visibilityExtent = directionalVisibilityPass.extent();
                const RenderGraph::TextureDesc visibilityDesc{
                    .extent = {visibilityExtent.width, visibilityExtent.height, 1},
                    .format = VK_FORMAT_R32_SFLOAT,
                    .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                             VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                             VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                    .aspect = VK_IMAGE_ASPECT_COLOR_BIT};
                graphDirectionalVisibility = viewportFrameGraph.importTexture(
                    "Directional visibility", directionalVisibilityPass.resultImage(currentFrame), visibilityDesc,
                    {.layout = VK_IMAGE_LAYOUT_UNDEFINED});
                // This graphics-queue compute pass only needs prepass outputs.
                // It overlaps GTAO on async compute.
                viewportFrameGraph.addPass(
                    "Directional visibility", RenderGraph::Queue::Graphics,
                    [&](RenderGraph::PassBuilder& builder) {
                        builder.read(graphShadowPageTable, RenderGraph::BufferUsage::StorageReadCompute);
                        builder.read(graphDepth, RenderGraph::TextureUsage::DepthReadCompute);
                        builder.read(graphViewNormal, RenderGraph::TextureUsage::SampledReadCompute);
                        builder.write(graphDirectionalVisibility, RenderGraph::TextureUsage::StorageWriteCompute);
                        builder.setFinalTextureState(
                            graphDirectionalVisibility,
                            {.stage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                             .access = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                             .layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                             .write = false});
                    },
                    [&](const VkCommandBuffer buffer) {
                        gpuTimestampProfiler.beginZone(buffer, currentFrame, directionalVisibilityProfileName);
                        directionalVisibilityPass.record(buffer, currentFrame, depthBuffer.imageView(),
                                                         depthBuffer.sampler(), gtaoViewNormalBuffer.imageView(),
                                                         gtaoViewNormalBuffer.sampler(), false);
                        gpuTimestampProfiler.endZone(buffer, currentFrame);
                    });
            }
            // Depth-bounded Forward+ light lists for Game View forward shading;
            // without them (MSAA, no lights) it walks the cluster lists.
            const bool tileLightsThisFrame =
                tileLightCulling.culling() && renderGameViewport && sceneFrameDataCache.data.lightCount != 0;
            if (tileLightsThisFrame) {
                viewportFrameGraph.addPass(
                    "Tile light culling", RenderGraph::Queue::Graphics,
                    [&](RenderGraph::PassBuilder& builder) {
                        builder.read(graphDepth, RenderGraph::TextureUsage::DepthReadCompute);
                        // The lists are outside the graph; the pass handles their barriers.
                        builder.setSideEffect();
                    },
                    [&](const VkCommandBuffer buffer) {
                        gpuTimestampProfiler.beginZone(buffer, currentFrame, tileLightCullingProfileName);
                        tileLightCulling.record(buffer, currentFrame, depthBuffer.imageView(), depthBuffer.sampler(),
                                                localLightBuffers[currentFrame].handle());
                        gpuTimestampProfiler.endZone(buffer, currentFrame);
                    });
            }
            // Stochastic local lighting (MegaLights-style), opt-in with
            // GAMENGINE_MEGALIGHTS=1: the tracer samples lights from the tile
            // lists and the deferred pass shades those samples, so every light
            // casts shadows at a cost independent of the light count. With a
            // few dozen lights per tile the exact loop (shadows from each
            // pixel's strongest lights) is faster, hence the default.
            const bool megaLightsThisFrame = megaLightsRequested() && tileLightsThisFrame && !msaa.enabled() &&
                                             deferredLocalLighting.stochasticReady();
            megaLightsTracedThisFrame = false;
            // Ray-traced local light shadows need only prepass outputs, the
            // TLAS and the cluster lists, so they run on graphics while GTAO
            // and SSGI occupy async compute; graphics otherwise idled there.
            // Whether to trace is decided with the TLAS update below, before
            // this graph records.
            bool localShadowsReadyThisFrame = false;
            if (localLightShadowPass.tracing() && graphViewNormal) {
                viewportFrameGraph.addPass(
                    "Local light shadows", RenderGraph::Queue::Graphics,
                    [&](RenderGraph::PassBuilder& builder) {
                        builder.read(graphDepth, RenderGraph::TextureUsage::DepthReadCompute);
                        builder.read(graphViewNormal, RenderGraph::TextureUsage::SampledReadCompute);
                        // The mask is outside the graph; the pass handles its layout.
                        builder.setSideEffect();
                    },
                    [&](const VkCommandBuffer buffer) {
                        if (!localShadowsReadyThisFrame)
                            return;
                        // The cluster scene is updated by the "Cluster RT scene"
                        // pass above; until its first build completes nothing
                        // is shadowed rather than reading a stale mask.
                        if (clusterShadowsRequested && !clusterScene.ready()) {
                            localLightShadowPass.clear(buffer, currentFrame);
                            return;
                        }
                        gpuTimestampProfiler.beginZone(buffer, currentFrame, localShadowProfileName);
                        localLightShadowPass.record(
                            buffer, currentFrame,
                            {.clusterScene = clusterShadowsRequested ? clusterScene.tlasAddress() : 0,
                             .tlas = clusterShadowsRequested ? VK_NULL_HANDLE : accelerationStructures.tlas(currentFrame),
                             .depth = depthBuffer.imageView(),
                             .depthSampler = depthBuffer.sampler(),
                             .normals = gtaoViewNormalBuffer.imageView(),
                             .normalSampler = gtaoViewNormalBuffer.sampler(),
                             .clusterRanges = clusteredLightRangeBuffers[currentFrame].handle(),
                             .clusterIndices = clusteredLightIndexBuffers[currentFrame].handle(),
                             .localLights = localLightBuffers[currentFrame].handle(),
                             .tileLights = tileLightCulling.lists(currentFrame)},
                            [&] {
                                // GAMENGINE_SHADOW_RAY_REUSE=<n> overrides the converged
                                // retrace interval (1 disables reuse).
                                LocalLightShadowPass::Settings settings;
                                settings.stochastic = megaLightsThisFrame;
                                if (const char* reuse = std::getenv("GAMENGINE_SHADOW_RAY_REUSE"))
                                    settings.convergedRayInterval =
                                        static_cast<std::uint32_t>(std::max(1, std::atoi(reuse)));
                                return settings;
                            }(),
                            [&] {
                                // The trace alone is what the backend choice compares.
                                gpuTimestampProfiler.endZone(buffer, currentFrame);
                                gpuTimestampProfiler.beginZone(buffer, currentFrame, localShadowDenoiseProfileName);
                            });
                        gpuTimestampProfiler.endZone(buffer, currentFrame);
                        megaLightsTracedThisFrame = megaLightsThisFrame;
                    });
            }
            // Forward lighting records after this graph. Its input pass waits
            // for GTAO while directional visibility can run on graphics.
            viewportFrameGraph.addPass(
                "Forward lighting inputs", RenderGraph::Queue::Graphics,
                [&](RenderGraph::PassBuilder& builder) {
                    builder.startNewBatch();
                    builder.read(graphShadowPageTable, RenderGraph::BufferUsage::StorageReadFragment);
                    builder.read(graphAo, RenderGraph::TextureUsage::SampledReadFragment);
                    if (graphSsgi)
                        builder.read(graphSsgi, RenderGraph::TextureUsage::SampledReadFragment);
                    if (graphDdgiIrradiance) {
                        builder.read(graphDdgiIrradiance, RenderGraph::TextureUsage::SampledReadFragment);
                        builder.read(graphDdgiDistance, RenderGraph::TextureUsage::SampledReadFragment);
                    }
                    if (graphDirectionalVisibility)
                        builder.read(graphDirectionalVisibility, RenderGraph::TextureUsage::SampledReadCompute);
                    builder.read(graphDepth, RenderGraph::TextureUsage::DepthReadCompute);
                    if (graphViewNormal)
                        builder.read(graphViewNormal, RenderGraph::TextureUsage::SampledReadCompute);
                    if (graphPreviousHiZ)
                        builder.read(graphPreviousHiZ, RenderGraph::TextureUsage::SampledReadCompute);
                    builder.setSideEffect();
                },
                {});
            // This frame's pyramid is first read by the next frame (culling
            // and VSM marking). Declared last, it becomes its own compute
            // batch that overlaps the raster-bound forward pass instead of
            // competing with GTAO and directional visibility for the SMs.
            // The viewport join is deferred to presentation for this reason.
            if (hizEnabled) {
                viewportFrameGraph.addPass(
                    "Hi-Z", gtaoQueue,
                    [&](RenderGraph::PassBuilder& builder) {
                        builder.startNewBatch();
                        builder.read(graphDepth, RenderGraph::TextureUsage::DepthReadCompute);
                        builder.write(graphHiZ, RenderGraph::TextureUsage::StorageWriteCompute);
                        builder.setFinalTextureState(graphHiZ, {.stage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                                                .access = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                                                                .layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                                                .write = false});
                    },
                    [&](const VkCommandBuffer buffer) {
                        // Timestamps stay on graphics; async compute gets a label only.
                        if (gtaoQueue == RenderGraph::Queue::Graphics)
                            gpuTimestampProfiler.beginZone(buffer, currentFrame, hiZProfileName);
                        else
                            gpuTimestampProfiler.beginLabel(buffer, hiZProfileName);
                        hiZPasses[currentFrame].record(buffer, hiZBuffers[currentFrame], VK_NULL_HANDLE, false);
                        if (gtaoQueue == RenderGraph::Queue::Graphics)
                            gpuTimestampProfiler.endZone(buffer, currentFrame);
                        else
                            gpuTimestampProfiler.endLabel(buffer);
                    });
                viewportFrameGraph.exportTexture(graphHiZ);
            }
            viewportFrameGraph.compile();
            profileRenderGraph(viewportFrameGraph, true);
            auto graphCommands =
                prepareGraphCommandBuffers(viewportFrameGraph, currentFrame, renderGraphCommandBuffers);
            RenderGraph::SubmissionContext context{};
            context.queues[static_cast<std::uint32_t>(RenderGraph::Queue::Graphics)] = vulkanDevice.graphicsQueue();
            context.queues[static_cast<std::uint32_t>(RenderGraph::Queue::AsyncCompute)] =
                renderPath == RenderPath::AsyncCompute && vulkanDevice.hasAsyncComputeQueue()
                    ? vulkanDevice.computeQueue()
                    : vulkanDevice.graphicsQueue();
            context.queues[static_cast<std::uint32_t>(RenderGraph::Queue::Transfer)] =
                renderPath == RenderPath::AsyncCompute && vulkanDevice.hasAsyncTransferQueue()
                    ? vulkanDevice.transferQueue()
                    : vulkanDevice.graphicsQueue();
            context.commandBuffers = std::move(graphCommands);
            context.queueTimelines[0] = renderGraphTimeline;
            context.queueTimelines[1] = renderGraphComputeTimeline;
            context.queueTimelines[2] = renderGraphTransferTimeline;
            context.nextQueueTimelineValues[0] = &renderGraphTimelineValue;
            context.nextQueueTimelineValues[1] = &renderGraphComputeTimelineValue;
            context.nextQueueTimelineValues[2] = &renderGraphTransferTimelineValue;
            context.uploadTimeline = uploadContext.timeline();
            context.deferredCompletion = &viewportGraphCompletion;

            const bool rtContactRequested = vulkanDevice.supportsRayQuery() && !msaa.enabled() && mainLightShadows &&
                                            rtContactShadowSettings.mode == ContactShadowMode::RayTraced &&
                                            rtContactShadowPass.resultView(currentFrame) != VK_NULL_HANDLE &&
                                            directionalVisibilityPass.resultView(currentFrame) != VK_NULL_HANDLE;
            const bool localShadowsRequested =
                localLightShadowPass.tracing() && renderGameViewport && localShadowCasters;
            // The classic triangle TLAS is needed only where shadows cannot
            // trace the cluster scene.
            const bool rtSceneRequested = rtContactRequested || (localShadowsRequested && !clusterShadowsRequested);
            if (rtSceneRequested && rayTracingBlasDirty && vertexBuffer.hasDeviceAddress() &&
                indexBuffer.hasDeviceAddress()) {
                std::vector<AccelerationStructureManager::MeshBuildInput>& meshes = rtMeshBuildInputs;
                meshes.clear();
                meshes.reserve(instanceBatches.size());
                std::unordered_set<AccelerationStructureManager::BlasKey, AccelerationStructureManager::BlasKeyHash>
                    builtSections;
                for (std::size_t batchIndex = 0; batchIndex < instanceBatches.size(); ++batchIndex) {
                    const InstanceBatch& batch = instanceBatches[batchIndex];
                    if (!batch.castShadow || batch.alphaMode != AlphaMode::Opaque || batch.mesh == nullptr ||
                        batch.indexCount < 3)
                        continue;
                    bool hasUndisplacedInstance = false;
                    for (const std::size_t renderableIndex : sceneGpu.batchRenderableIndices[batchIndex]) {
                        if (renderables[renderableIndex].displacementBoundsPadding <= 1.0e-6F) {
                            hasUndisplacedInstance = true;
                            break;
                        }
                    }
                    if (!hasUndisplacedInstance)
                        continue;
                    const auto allocationIt = geometryHeapAllocations.find(batch.mesh);
                    if (allocationIt == geometryHeapAllocations.end())
                        continue;
                    const auto& allocation = allocationIt->second;
                    const AccelerationStructureManager::BlasKey section{batch.mesh, batch.firstIndex,
                                                                        batch.indexCount};
                    if (!builtSections.insert(section).second)
                        continue;
                    // Every section of a mesh joins that mesh's single BLAS.
                    meshes.push_back(
                        {{batch.mesh, 0, 0}, vertexBuffer.deviceAddress(),
                         indexBuffer.deviceAddress() + VkDeviceAddress(batch.firstIndex) * sizeof(std::uint32_t),
                         allocation.firstVertex, allocation.vertexCount, batch.indexCount});
                }
                accelerationStructures.rebuildBlases(commandBuffer, meshes);
                rayTracingBlasDirty = false;
                rtTlasInputDirty = true;
            }
            if (rtSceneRequested && !rayTracingBlasDirty) {
                const std::uint64_t topologyRevision = registry.renderTopologyRevision();
                // Camera/light-only motion does not change RT geometry.
                // Instance updates publish rtTlasInputDirty after resolving
                // world transforms, including motion inherited from parents.
                const bool inputChanged = rtTlasInputDirty || topologyRevision != lastRtTlasTopologyRevision;
                if (inputChanged) {
                    rtTlasInstances.clear();
                    // One instance per mesh placement: its sections share
                    // the mesh's BLAS and the object's transform.
                    std::unordered_set<std::uint64_t> placedMeshes;
                    for (std::size_t batchIndex = 0; batchIndex < instanceBatches.size(); ++batchIndex) {
                        const InstanceBatch& batch = instanceBatches[batchIndex];
                        // Masked surfaces need candidate alpha testing before
                        // they can join this opaque-only TLAS.
                        if (!batch.castShadow || batch.alphaMode != AlphaMode::Opaque || batch.mesh == nullptr ||
                            batch.indexCount < 3)
                            continue;
                        for (const std::size_t renderableIndex : sceneGpu.batchRenderableIndices[batchIndex]) {
                            // The BLAS contains source vertices. Displaced
                            // instances need cooked or updated RT geometry.
                            if (renderables[renderableIndex].displacementBoundsPadding > 1.0e-6F)
                                continue;
                            const RendererInstanceData& source = instanceModels[renderableIndex];
                            const glm::quat q{source.rotation.w, source.rotation.x, source.rotation.y,
                                              source.rotation.z};
                            glm::mat4 model = glm::mat4_cast(q);
                            model[0] *= source.scaleBase.x;
                            model[1] *= source.scaleBase.y;
                            model[2] *= source.scaleBase.z;
                            model[3] = glm::vec4(source.positionMaterial.x, source.positionMaterial.y,
                                                 source.positionMaterial.z, 1.0F);
                            AccelerationStructureManager::InstanceBuildInput input{};
                            input.meshKey = {batch.mesh, 0, 0};
                            const GPUSceneInstanceId instanceId = renderables[renderableIndex].renderProxy.instance;
                            assert(instanceId < (1U << 24U));
                            input.mask = 0x01;
                            input.customIndex = instanceId;
                            for (std::uint32_t row = 0; row < 3; ++row)
                                for (std::uint32_t column = 0; column < 4; ++column)
                                    input.transform[row * 4 + column] = model[column][row];
                            std::uint64_t placement = std::hash<const void*>{}(batch.mesh);
                            for (const float value : input.transform)
                                placement = (placement ^ std::hash<float>{}(value)) * 1099511628211ULL;
                            if (!placedMeshes.insert(placement).second)
                                continue;
                            rtTlasInstances.push_back(input);
                        }
                    }
                    lastRtTlasTopologyRevision = topologyRevision;
                    rtTlasInputDirty = false;
                    rtTlasUpdatePending.fill(true);
                }
                // Compacts the BLASes a few frames after their build; a
                // finished compaction leaves every TLAS to rebuild below.
                accelerationStructures.advanceCompaction(commandBuffer);
                if (rtTlasUpdatePending[currentFrame] || !accelerationStructures.built(currentFrame)) {
                    gpuTimestampProfiler.beginZone(commandBuffer, currentFrame, rtTlasProfileName);
                    accelerationStructures.updateTlas(commandBuffer, currentFrame, rtTlasInstances);
                    rtTlasUpdatePending[currentFrame] = false;
                    gpuTimestampProfiler.endZone(commandBuffer, currentFrame);
                }
            }
            // Publish the feature flag before any submission can read the
            // frame UBO. The depth/GTAO/visibility graph shares this allocation
            // with RTCS and forward lighting; a later host write races its GPU reads.
            const bool rtContactReadyThisFrame =
                rtContactRequested && !rayTracingBlasDirty && accelerationStructures.built(currentFrame);
            const float rtContactEnabled = rtContactReadyThisFrame ? 1.0F : 0.0F;
            uniformBuffers[currentFrame].update(
                &rtContactEnabled, sizeof(rtContactEnabled),
                offsetof(UniformBufferObject, clusterZScaleBiasEnvironmentMipRtContact) + sizeof(float) * 3);
            localShadowsReadyThisFrame =
                clusterShadowsRequested ||
                (localShadowsRequested && !rayTracingBlasDirty && accelerationStructures.built(currentFrame));
            // Deferred local lights need the tile lists of this frame.
            deferredLocalThisFrame = tileLightsThisFrame && deferredLocalLighting.ready() && !msaa.enabled();
            // Same condition as deferLocalLights in forward_pbr.slang: the
            // PBR programs may then drop their own local light walk.
            lightingForwardPass.useDeferredLocalShaders(deferredLocalThisFrame &&
                                                        pbrDebugView == PbrDebugView::FinalLighting);
            const std::uint32_t screenSpaceEffects = gameScreenSpaceEffects | (localShadowsReadyThisFrame ? 8U : 0U) |
                                                     (tileLightsThisFrame ? 16U : 0U) |
                                                     (deferredLocalThisFrame ? 32U : 0U);
            uniformBuffers[currentFrame].update(&screenSpaceEffects, sizeof(screenSpaceEffects),
                                                offsetof(UniformBufferObject, screenSpaceEffects));

            if (vkEndCommandBuffer(commandBuffer) != VK_SUCCESS)
                throw std::runtime_error("Could not end graphics commands before viewport RenderGraph");
            const auto pendingUploadValue = [this](const std::uint64_t value) {
                return value != 0 && uploadContext.completedValue() < value ? value : 0;
            };
            const std::uint64_t earlyUploadValue = pendingUploadValue(legacyUploadValue());
            const VkSemaphoreSubmitInfo earlyUploadWait{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
                                                        .semaphore = uploadContext.timeline(),
                                                        .value = earlyUploadValue,
                                                        .stageMask = legacyUploadStages};
            const VkCommandBufferSubmitInfo earlyCommand{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
                                                         .commandBuffer = commandBuffer};
            const VkSubmitInfo2 earlySubmit{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
                                            .waitSemaphoreInfoCount = earlyUploadValue != 0 ? 1U : 0U,
                                            .pWaitSemaphoreInfos = earlyUploadValue != 0 ? &earlyUploadWait : nullptr,
                                            .commandBufferInfoCount = 1,
                                            .pCommandBufferInfos = &earlyCommand};
            if (vkQueueSubmit2(vulkanDevice.graphicsQueue(), 1, &earlySubmit, VK_NULL_HANDLE) != VK_SUCCESS)
                throw std::runtime_error("Could not submit graphics commands before viewport RenderGraph");
            viewportFrameGraph.recordAndSubmit(context);
            commandBuffer = postViewportGraphicsCommandBuffers.at(currentFrame);
            const VkCommandBufferBeginInfo postBegin{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            if (vkBeginCommandBuffer(commandBuffer, &postBegin) != VK_SUCCESS)
                throw std::runtime_error("Could not begin graphics commands after viewport RenderGraph");
            hdrBufferInitialized = true;
            if (hizEnabled) {
                hiZViewProjections[currentFrame] = cameraController.camera()->projectionMatrix().native() *
                                                   cameraController.camera()->viewMatrix().native();
                hiZValid[currentFrame] = true;
                VsmFeedbackKey depthKey;
                depthKey.viewProjection = cameraController.camera()->unjitteredProjectionMatrix().native() *
                                          cameraController.camera()->viewMatrix().native();
                depthKey.sceneRevision = registry.renderTopologyRevision();
                depthKey.transformRevision = depthContentRevision;
                depthKey.deformationRevision = grassDeformationVersion;
                depthKey.windRevision = sceneFrameDataCache.windRevision;
                depthKey.animationFrame =
                    sceneFrameDataCache.hasWind && !sceneGpu.grassInstances.empty() ? submittedFrameValue / 2U : 0;
                depthKey.width = swapchain.extent().width;
                depthKey.height = swapchain.extent().height;
                hiZFeedbackKeys[currentFrame] = depthKey;
            }

            if (rtContactReadyThisFrame) {
                gpuTimestampProfiler.beginZone(commandBuffer, currentFrame, rtContactProfileName);
                rtContactShadowPass.record(
                    commandBuffer, currentFrame, accelerationStructures.tlas(currentFrame), depthBuffer.imageView(),
                    depthBuffer.sampler(), gtaoViewNormalBuffer.imageView(), gtaoViewNormalBuffer.sampler(),
                    directionalVisibilityPass.resultView(currentFrame),
                    directionalVisibilityPass.resultSampler(currentFrame), rtContactShadowSettings);
                rtContactActive = true;
                gpuTimestampProfiler.endZone(commandBuffer, currentFrame);
            }
            gpuTimestampProfiler.beginZone(commandBuffer, currentFrame, forwardProfileName);
            gpuTimestampProfiler.beginZone(commandBuffer, currentFrame, forwardOpaqueProfileName);
            // The single-sample lighting pass always has the deferred local
            // light target; its PBR programs clear it when not deferring.
            if (!msaa.enabled())
                deferredLocalLighting.prepare(commandBuffer);
            lightingForwardPass.begin(
                commandBuffer, msaa.enabled() ? msaa.colorImageView() : hdrBuffer.imageView(), depthBuffer.imageView(),
                // Velocity is already final from the depth prepass; passing
                // the view here would clear and rewrite it a second time.
                VK_NULL_HANDLE,
                VK_NULL_HANDLE, msaa.enabled() ? hdrBuffer.imageView() : VK_NULL_HANDLE,
                msaa.enabled() ? hiZDepthBuffer.imageView() : VK_NULL_HANDLE, swapchain.extent(),
                shadowPass.descriptorSet(currentFrame), vertexBuffer.handle(), instanceBuffers[currentFrame].handle(),
                indexBuffer.handle(), msaa.enabled() ? VK_NULL_HANDLE : deferredLocalLighting.surfaceData());
            if (meshletForwardReady)
                vkCmdBindIndexBuffer(commandBuffer, indexBuffer.handle(), 0, VK_INDEX_TYPE_UINT32);
            for (std::uint32_t shader = 0; shader < MaterialProgramSlotCount; ++shader) {
                if (!activeShaderSlots.test(shader))
                    continue;
                if (isCompositedProgramSlot(shader))
                    continue;
                const auto countOffset = static_cast<VkDeviceSize>(shader) * sizeof(std::uint32_t);
                if (meshShaderForward) {
                    lightingForwardPass.drawMeshShaderMaterial(commandBuffer, shadowPass.descriptorSet(currentFrame),
                                                               meshletDrawSets[currentFrame], shader,
                                                               meshletDrawCommandBuffers[currentFrame].handle());
                } else if (meshletForwardReady) {
                    const auto commandOffset = meshletCommandOffset(shader);
                    lightingForwardPass.drawMeshletMaterial(commandBuffer, shadowPass.descriptorSet(currentFrame),
                                                            shader, meshletIndexedIndirectDraws[currentFrame],
                                                            commandOffset, countOffset);
                } else {
                    const auto commandOffset =
                        binnedCommandOffset(static_cast<std::uint32_t>(shader), false);
                    const auto twoSidedCommandOffset = binnedCommandOffset(static_cast<std::uint32_t>(shader), true);
                    lightingForwardPass.drawMaterial(commandBuffer, shadowPass.descriptorSet(currentFrame), shader,
                                                     indirectDraws[currentFrame], commandOffset, countOffset);
                    lightingForwardPass.drawMaterial(commandBuffer, shadowPass.descriptorSet(currentFrame), shader,
                                                     foliageIndirectDraws[currentFrame], twoSidedCommandOffset, countOffset,
                                                     true);
                }
            }
            if (meshletForwardReady)
                vkCmdBindIndexBuffer(commandBuffer, indexBuffer.handle(), 0, VK_INDEX_TYPE_UINT32);
            if (meshletSkipsFoliage) {
                for (std::uint32_t shader = 0; shader < MaterialProgramSlotCount; ++shader) {
                    if (!activeShaderSlots.test(shader) || isCompositedProgramSlot(shader))
                        continue;
                    lightingForwardPass.drawMaterial(
                        commandBuffer, shadowPass.descriptorSet(currentFrame), shader,
                        foliageIndirectDraws[currentFrame],
                        binnedCommandOffset(static_cast<std::uint32_t>(shader), true),
                        static_cast<VkDeviceSize>(shader) * sizeof(std::uint32_t), true);
                }
            }
            if (!sceneGpu.grassInstances.empty()) {
                const auto& lists = grassRenderLists[currentFrame];
                Culling::IndexedIndirectDrawCount grassDraw;
                grassDraw.create(lists.mainIndirect.handle(), lists.mainDrawCount.handle(),
                                 static_cast<uint32_t>(std::max<std::size_t>(1, sceneGpu.grassClusters.size())));
                shadowPass.setGrassVisibleInstances(currentFrame, lists.drawInstances[0].handle());
                lightingForwardPass.drawGrass(commandBuffer, shadowPass.grassDescriptorSet(currentFrame), grassDraw);
            }
            gpuTimestampProfiler.endZone(commandBuffer, currentFrame);
            if (!msaa.enabled()) {
                // The opaque surfaces end here: the deferred pass adds their
                // local lights, then sky and particles continue on the lit
                // color with HDR and depth only (their pipelines have no
                // deferred-light target).
                ForwardPass::end(commandBuffer);
                if (deferredLocalThisFrame) {
                    gpuTimestampProfiler.beginZone(commandBuffer, currentFrame, deferredLocalProfileName);
                    deferredLocalLighting.record(
                        commandBuffer, currentFrame,
                        {.depth = depthBuffer.imageView(),
                         .depthSampler = depthBuffer.sampler(),
                         .hdr = hdrBuffer.image(),
                         .hdrView = hdrBuffer.imageView(),
                         .localLights = localLightBuffers[currentFrame].handle(),
                         .tileLights = tileLightCulling.lists(currentFrame),
                         .localShadows = localLightShadowPass.resultView(currentFrame),
                         .localShadowSampler = localLightShadowPass.resultSampler(currentFrame),
                         .lightSamples = megaLightsTracedThisFrame ? localLightShadowPass.sampleView() : VK_NULL_HANDLE,
                         .lightSampleWeights =
                             megaLightsTracedThisFrame ? localLightShadowPass.sampleWeightView() : VK_NULL_HANDLE});
                    gpuTimestampProfiler.endZone(commandBuffer, currentFrame);
                }
                VkRenderingAttachmentInfo resumedColor{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
                resumedColor.imageView = hdrBuffer.imageView();
                resumedColor.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                resumedColor.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
                resumedColor.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                VkRenderingAttachmentInfo resumedDepth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
                resumedDepth.imageView = depthBuffer.imageView();
                resumedDepth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
                resumedDepth.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
                resumedDepth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                VkRenderingInfo resumed{VK_STRUCTURE_TYPE_RENDERING_INFO};
                resumed.renderArea.extent = swapchain.extent();
                resumed.layerCount = 1;
                resumed.colorAttachmentCount = 1;
                resumed.pColorAttachments = &resumedColor;
                resumed.pDepthAttachment = &resumedDepth;
                vkCmdBeginRendering(commandBuffer, &resumed);
                const VkViewport viewport{0.0F, 0.0F, static_cast<float>(swapchain.extent().width),
                                          static_cast<float>(swapchain.extent().height), 0.0F, 1.0F};
                const VkRect2D scissor{{0, 0}, swapchain.extent()};
                vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
                vkCmdSetScissor(commandBuffer, 0, 1, &scissor);
            }
            skyPass.record(commandBuffer, currentFrame);
            // Water must be composited before alpha-blended particles.  A
            // particle does not write depth, so rendering it into the opaque
            // source would let the later water pass incorrectly cover it.
            const bool hasLegacyWater = activeShaderSlots.test(materialShaderIndex(MaterialShader::Water));
            const bool hasVirtualWater = virtualWaterRenderer.active();
            const bool needsWaterComposite = hasLegacyWater || hasVirtualWater;
            if (!needsWaterComposite) {
                if (particleSystem && cameraController.camera()) {
                    Particles::ParticleFrameData particleFrame{cameraController.camera()->projectionMatrix() *
                                                                         cameraController.camera()->viewMatrix(),
                                                                     particlePreviousGameViewProjection,
                                                                     cameraController.camera()->right(),
                                                                     0.0F,
                                                                     cameraController.camera()->up(),
                                                                     0.0F,
                                                                     particlePreviousGameCameraRight,
                                                                     0.0F,
                                                                     particlePreviousGameCameraUp,
                                                                     0.0F,
                                                                     static_cast<float>(Time::deltaTime())};
                    particleSystem->recordRender(commandBuffer, particleFrame, particlePipeline.handle(),
                                                 particlePipeline.layout(), currentFrame, false);
                }
            }
            ForwardPass::end(commandBuffer);
            gpuTimestampProfiler.endZone(commandBuffer, currentFrame);
            // Dynamic rendering has no render-pass finalLayout.  The post graph
            // samples these images, so make their real state match its import.
            if (!needsWaterComposite) {
                VkImageMemoryBarrier2 hdrToSampled{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
                hdrToSampled.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
                hdrToSampled.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
                hdrToSampled.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
                hdrToSampled.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
                hdrToSampled.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                hdrToSampled.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                hdrToSampled.image = hdrBuffer.image();
                hdrToSampled.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                VkImageMemoryBarrier2 velocityToSampled = hdrToSampled;
                velocityToSampled.image = velocityBuffer.image();
                const std::array postLightingTransitions{hdrToSampled, velocityToSampled};
                const VkDependencyInfo postLightingDependency{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                                                              .imageMemoryBarrierCount =
                                                                  antialiasingLevel == AntialiasingLevel::TAA ? 2U : 1U,
                                                              .pImageMemoryBarriers = postLightingTransitions.data()};
                vkCmdPipelineBarrier2(commandBuffer, &postLightingDependency);
            }
            if (needsWaterComposite) {
                const VkImageMemoryBarrier2 barriersBefore[] = {
                    {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                     nullptr,
                     VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                     VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                     VK_ACCESS_2_TRANSFER_READ_BIT,
                     VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     VK_QUEUE_FAMILY_IGNORED,
                     VK_QUEUE_FAMILY_IGNORED,
                     hdrBuffer.image(),
                     {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}},
                    {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                     nullptr,
                     opaqueSceneColorInitialized ? VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT : VK_PIPELINE_STAGE_2_NONE,
                     opaqueSceneColorInitialized ? VK_ACCESS_2_SHADER_SAMPLED_READ_BIT : 0,
                     VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                     VK_ACCESS_2_TRANSFER_WRITE_BIT,
                     opaqueSceneColorInitialized ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_QUEUE_FAMILY_IGNORED,
                     VK_QUEUE_FAMILY_IGNORED,
                     opaqueSceneColor.image(),
                     {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}},
                };
                VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
                dependency.imageMemoryBarrierCount = std::size(barriersBefore);
                dependency.pImageMemoryBarriers = barriersBefore;
                vkCmdPipelineBarrier2(commandBuffer, &dependency);
                const VkImageCopy copy{{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                       {0, 0, 0},
                                       {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                       {0, 0, 0},
                                       {swapchain.extent().width, swapchain.extent().height, 1}};
                vkCmdCopyImage(commandBuffer, hdrBuffer.image(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               opaqueSceneColor.image(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
                const VkImageMemoryBarrier2 barriersAfter[] = {
                    {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                     nullptr,
                     VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                     VK_ACCESS_2_TRANSFER_READ_BIT,
                     VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                     VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                     VK_QUEUE_FAMILY_IGNORED,
                     VK_QUEUE_FAMILY_IGNORED,
                     hdrBuffer.image(),
                     {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}},
                    {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                     nullptr,
                     VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                     VK_ACCESS_2_TRANSFER_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                     VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                     VK_QUEUE_FAMILY_IGNORED,
                     VK_QUEUE_FAMILY_IGNORED,
                     opaqueSceneColor.image(),
                     {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}},
                };
                dependency.imageMemoryBarrierCount = std::size(barriersAfter);
                dependency.pImageMemoryBarriers = barriersAfter;
                vkCmdPipelineBarrier2(commandBuffer, &dependency);
                opaqueSceneColorInitialized = true;
                const auto commandOffset = binnedCommandOffset(static_cast<std::uint32_t>(materialShaderIndex(MaterialShader::Water)), false);
                const auto countOffset =
                    static_cast<VkDeviceSize>(materialShaderIndex(MaterialShader::Water)) * sizeof(std::uint32_t);
                if (hasVirtualWater) {
                    if (cameraController.camera()) {
                        virtualWaterRenderer.updateUnderwaterState(currentFrame, registry,
                                                                   cameraController.camera()->position(),
                                                                   static_cast<float>(Time::elapsedTime()));
                    }
                    static const ProfileNameId waterPrepassProfileName =
                        Profiler::registerName("Virtual Water Prepass");
                    static const ProfileNameId waterShadeProfileName = Profiler::registerName("Virtual Water Shading");
                    gpuTimestampProfiler.beginZone(commandBuffer, currentFrame, waterPrepassProfileName);
                    virtualWaterRenderer.recordPrepass(
                        commandBuffer, currentFrame, shadowPass.descriptorSet(currentFrame), vertexBuffer.handle(),
                        indexBuffer.handle(), indirectDraws[currentFrame], commandOffset, countOffset);
                    virtualWaterPreparedThisFrame = virtualWaterRenderer.active();
                    gpuTimestampProfiler.endZone(commandBuffer, currentFrame);
                    gpuTimestampProfiler.beginZone(commandBuffer, currentFrame, waterShadeProfileName);
                    virtualWaterRenderer.recordAdaptiveShading(commandBuffer, currentFrame,
                                                               shadowPass.descriptorSet(currentFrame));
                    virtualWaterRenderer.recordComposite(commandBuffer, currentFrame,
                                                         shadowPass.descriptorSet(currentFrame));
                    gpuTimestampProfiler.endZone(commandBuffer, currentFrame);
                }
                VkImageMemoryBarrier2 waterColorTransition{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
                waterColorTransition.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
                waterColorTransition.srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
                waterColorTransition.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
                waterColorTransition.dstAccessMask =
                    VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
                waterColorTransition.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                waterColorTransition.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                waterColorTransition.image = hdrBuffer.image();
                waterColorTransition.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                VkImageMemoryBarrier2 waterVelocityTransition = waterColorTransition;
                waterVelocityTransition.image = velocityBuffer.image();
                std::array waterTransitions{waterColorTransition, waterVelocityTransition};
                VkDependencyInfo waterDependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
                waterDependency.imageMemoryBarrierCount = antialiasingLevel == AntialiasingLevel::TAA ? 2U : 1U;
                waterDependency.pImageMemoryBarriers = waterTransitions.data();
                vkCmdPipelineBarrier2(commandBuffer, &waterDependency);
                waterPass.begin(commandBuffer, msaa.enabled() ? msaa.colorImageView() : hdrBuffer.imageView(),
                                depthBuffer.imageView(), velocityBuffer.imageView(),
                                msaa.enabled() ? hdrBuffer.imageView() : VK_NULL_HANDLE,
                                msaa.enabled() ? hiZDepthBuffer.imageView() : VK_NULL_HANDLE, swapchain.extent(),
                                shadowPass.descriptorSet(currentFrame), currentFrame, vertexBuffer.handle(),
                                indexBuffer.handle());
                // When Virtual Water is active it owns ocean, lake and river shading.
                // Keep this compatible render pass only for particles; drawing water here
                // would shade authored bodies twice.
                if (hasLegacyWater && !hasVirtualWater) {
                    waterPass.draw(commandBuffer, shadowPass.descriptorSet(currentFrame), indirectDraws[currentFrame],
                                   commandOffset, countOffset);
                }
                // Particles use a compatible pipeline and can be composited
                // after water. The outline pipeline belongs to the lighting
                // render pass and was recorded before ending that pass.
                if (particleSystem && cameraController.camera()) {
                    Particles::ParticleFrameData particleFrame{cameraController.camera()->projectionMatrix() *
                                                                         cameraController.camera()->viewMatrix(),
                                                                     particlePreviousGameViewProjection,
                                                                     cameraController.camera()->right(),
                                                                     0.0F,
                                                                     cameraController.camera()->up(),
                                                                     0.0F,
                                                                     particlePreviousGameCameraRight,
                                                                     0.0F,
                                                                     particlePreviousGameCameraUp,
                                                                     0.0F,
                                                                     static_cast<float>(Time::deltaTime())};
                    particleSystem->recordRender(commandBuffer, particleFrame, particlePipeline.handle(),
                                                 particlePipeline.layout(), currentFrame, false);
                }
                WaterPass::end(commandBuffer);
                for (auto& transition : waterTransitions) {
                    transition.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
                    transition.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
                    transition.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
                    transition.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
                    transition.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                    transition.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                }
                vkCmdPipelineBarrier2(commandBuffer, &waterDependency);
            }
            // Glass refracts and reflects everything above: opaque, sky,
            // water and particles. It leaves HDR sampled for post-processing.
            if (activeShaderSlots.test(GlassProgramSlot) && !glassPass.created() && glassPassInfo.device != VK_NULL_HANDLE)
                glassPass.create(glassPassInfo);
            if (activeShaderSlots.test(GlassProgramSlot) && glassPass.created()) {
                static const ProfileNameId glassProfileName = Profiler::registerName("Glass");
                gpuTimestampProfiler.beginZone(commandBuffer, currentFrame, glassProfileName);
                glassPass.record(
                    commandBuffer,
                    {.color = hdrBuffer.image(),
                     .colorView = hdrBuffer.imageView(),
                     .depth = (msaa.enabled() ? hiZDepthBuffer : depthBuffer).image(),
                     .sceneDescriptorSet = shadowPass.descriptorSet(currentFrame),
                     .vertexBuffer = vertexBuffer.handle(),
                     .indexBuffer = indexBuffer.handle(),
                     .singleSided = &indirectDraws[currentFrame],
                     .twoSided = &foliageIndirectDraws[currentFrame],
                     .commandOffset = binnedCommandOffset(static_cast<std::uint32_t>(GlassProgramSlot), false),
                     .twoSidedCommandOffset = binnedCommandOffset(static_cast<std::uint32_t>(GlassProgramSlot), true),
                     .countOffset = static_cast<VkDeviceSize>(GlassProgramSlot) * sizeof(std::uint32_t)});
                gpuTimestampProfiler.endZone(commandBuffer, currentFrame);
            }
        }

        if (cullingZoneOpen) {
            gpuTimestampProfiler.endZone(commandBuffer, currentFrame);
            cullingZoneOpen = false;
        }
        static const ProfileNameId sceneViewProfileName = Profiler::registerName("Scene View");
        if (renderSceneViewport)
            gpuTimestampProfiler.beginZone(commandBuffer, currentFrame, sceneViewProfileName);
        if (renderSceneViewport) {
            // The prior Scene View image was sampled by ImGui. Make those
            // reads visible before the cache pass changes it back into a
            // color attachment. Dynamic rendering requires both explicit
            // layout transitions.
            if (sceneViewportImageInitialized) {
                VkImageMemoryBarrier2 sampledToColor{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
                sampledToColor.srcStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
                sampledToColor.srcAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
                sampledToColor.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
                sampledToColor.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
                sampledToColor.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                sampledToColor.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                sampledToColor.image = sceneViewportTarget.color().image();
                sampledToColor.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
                dependency.imageMemoryBarrierCount = 1;
                dependency.pImageMemoryBarriers = &sampledToColor;
                vkCmdPipelineBarrier2(commandBuffer, &dependency);
            }
            VkImageMemoryBarrier2 depthToAttachment{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
            depthToAttachment.srcStageMask =
                sceneViewportDepthInitialized
                    ? (VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT)
                    : VK_PIPELINE_STAGE_2_NONE;
            depthToAttachment.srcAccessMask = sceneViewportDepthInitialized ? VK_ACCESS_2_SHADER_SAMPLED_READ_BIT : 0;
            depthToAttachment.dstStageMask =
                VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
            depthToAttachment.dstAccessMask =
                VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            depthToAttachment.oldLayout = sceneViewportDepthInitialized
                                              ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
                                              : VK_IMAGE_LAYOUT_UNDEFINED;
            depthToAttachment.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            depthToAttachment.image = sceneViewportDepthBuffer.image();
            depthToAttachment.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
            VkDependencyInfo depthDependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            depthDependency.imageMemoryBarrierCount = 1;
            depthDependency.pImageMemoryBarriers = &depthToAttachment;
            vkCmdPipelineBarrier2(commandBuffer, &depthDependency);
            // Scene View has a separate frustum and therefore needs its own
            // indirect list. The game camera's list must not hide objects
            // which are visible from the editor camera.
            sceneGpuCullingPasses[currentFrame].recordBinned(
                commandBuffer, static_cast<std::uint32_t>(gpuObjects.size()), MaterialProgramSlotCount);
            sceneFoliageGpuCullingPasses[currentFrame].recordBinned(
                commandBuffer, static_cast<std::uint32_t>(gpuObjects.size()), MaterialProgramSlotCount);

            // Depth prepass: depth plus the view normals that directional
            // visibility reads. Velocity is written only because the shared
            // prepass programs output it; nothing samples it.
            {
                const VkImageMemoryBarrier2 toAttachment[] = {
                    {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, nullptr, VK_PIPELINE_STAGE_2_NONE, 0,
                     VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                     VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_QUEUE_FAMILY_IGNORED,
                     VK_QUEUE_FAMILY_IGNORED, sceneViewportVelocity.image(), {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}},
                    // The previous Scene View frame's visibility pass read these.
                    {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, nullptr, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, 0,
                     VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                     VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_QUEUE_FAMILY_IGNORED,
                     VK_QUEUE_FAMILY_IGNORED, sceneViewportNormals.image(), {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}},
                };
                VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
                dependency.imageMemoryBarrierCount = std::size(toAttachment);
                dependency.pImageMemoryBarriers = toAttachment;
                vkCmdPipelineBarrier2(commandBuffer, &dependency);
            }
            sceneViewportDepthPrepass.begin(commandBuffer, sceneViewportTarget.color().imageView(),
                                            sceneViewportDepthBuffer.imageView(), sceneViewportVelocity.imageView(),
                                            sceneViewportNormals.imageView(), VK_NULL_HANDLE, VK_NULL_HANDLE,
                                            sceneViewportTarget.extent(),
                                            sceneDescriptorPass.descriptorSet(currentFrame), vertexBuffer.handle(),
                                            instanceBuffers[currentFrame].handle(), indexBuffer.handle());
            for (std::uint32_t shader = 0; shader < MaterialProgramSlotCount; ++shader) {
                if (!activeShaderSlots.test(shader) || isCompositedProgramSlot(shader))
                    continue;
                const auto commandOffset =
                    binnedCommandOffset(static_cast<std::uint32_t>(shader), false);
                const auto twoSidedCommandOffset = binnedCommandOffset(static_cast<std::uint32_t>(shader), true);
                const auto countOffset = static_cast<VkDeviceSize>(shader) * sizeof(std::uint32_t);
                sceneViewportDepthPrepass.drawMaterial(commandBuffer, sceneDescriptorPass.descriptorSet(currentFrame),
                                                       shader, sceneIndirectDraws[currentFrame], commandOffset,
                                                       countOffset);
                sceneViewportDepthPrepass.drawMaterial(commandBuffer, sceneDescriptorPass.descriptorSet(currentFrame),
                                                       shader, sceneFoliageIndirectDraws[currentFrame], twoSidedCommandOffset,
                                                       countOffset, true);
            }
            if (!sceneGpu.grassInstances.empty()) {
                const auto& lists = sceneGrassRenderLists[currentFrame];
                Culling::IndexedIndirectDrawCount grassDraw;
                grassDraw.create(lists.mainIndirect.handle(), lists.mainDrawCount.handle(),
                                 static_cast<uint32_t>(std::max<std::size_t>(1, sceneGpu.grassClusters.size())));
                sceneDescriptorPass.setGrassVisibleInstances(currentFrame, lists.drawInstances[0].handle());
                sceneViewportDepthPrepass.drawGrass(commandBuffer, sceneDescriptorPass.grassDescriptorSet(currentFrame),
                                                    grassDraw);
            }
            ForwardPass::end(commandBuffer);
            {
                VkImageMemoryBarrier2 depthToRead{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
                depthToRead.srcStageMask =
                    VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
                depthToRead.srcAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
                depthToRead.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                                           VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                                           VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
                depthToRead.dstAccessMask =
                    VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
                depthToRead.oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
                depthToRead.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
                depthToRead.image = sceneViewportDepthBuffer.image();
                depthToRead.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
                VkImageMemoryBarrier2 normalsToRead{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
                normalsToRead.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
                normalsToRead.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
                normalsToRead.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                normalsToRead.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
                normalsToRead.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                normalsToRead.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                normalsToRead.image = sceneViewportNormals.image();
                normalsToRead.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                const VkImageMemoryBarrier2 prepassToRead[] = {depthToRead, normalsToRead};
                VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
                dependency.imageMemoryBarrierCount = std::size(prepassToRead);
                dependency.pImageMemoryBarriers = prepassToRead;
                vkCmdPipelineBarrier2(commandBuffer, &dependency);
            }
            sceneDirectionalVisibilityPass.record(commandBuffer, currentFrame, sceneViewportDepthBuffer.imageView(),
                                                  sceneViewportDepthBuffer.sampler(), sceneViewportNormals.imageView(),
                                                  sceneViewportNormals.sampler());
            sceneForwardPass.begin(commandBuffer, sceneViewportTarget.color().imageView(),
                                   sceneViewportDepthBuffer.imageView(), VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE,
                                   VK_NULL_HANDLE, sceneViewportTarget.extent(),
                                   sceneDescriptorPass.descriptorSet(currentFrame), vertexBuffer.handle(),
                                   instanceBuffers[currentFrame].handle(), indexBuffer.handle());
            for (std::uint32_t shader = 0; shader < MaterialProgramSlotCount; ++shader) {
                if (!activeShaderSlots.test(shader))
                    continue;
                // Scene View will receive the same dedicated post-opaque
                // WaterPass when its viewport render graph is split. Do
                // not accidentally run water through its opaque pass.
                if (isCompositedProgramSlot(shader))
                    continue;
                const auto commandOffset =
                    binnedCommandOffset(static_cast<std::uint32_t>(shader), false);
                const auto twoSidedCommandOffset = binnedCommandOffset(static_cast<std::uint32_t>(shader), true);
                const auto countOffset = static_cast<VkDeviceSize>(shader) * sizeof(std::uint32_t);
                sceneForwardPass.drawMaterial(commandBuffer, sceneDescriptorPass.descriptorSet(currentFrame), shader,
                                              sceneIndirectDraws[currentFrame], commandOffset, countOffset);
                sceneForwardPass.drawMaterial(commandBuffer, sceneDescriptorPass.descriptorSet(currentFrame), shader,
                                              sceneFoliageIndirectDraws[currentFrame], twoSidedCommandOffset, countOffset,
                                              true);
            }
            if (!sceneGpu.grassInstances.empty()) {
                const auto& lists = sceneGrassRenderLists[currentFrame];
                Culling::IndexedIndirectDrawCount grassDraw;
                grassDraw.create(lists.mainIndirect.handle(), lists.mainDrawCount.handle(),
                                 static_cast<uint32_t>(std::max<std::size_t>(1, sceneGpu.grassClusters.size())));
                sceneDescriptorPass.setGrassVisibleInstances(currentFrame, lists.drawInstances[0].handle());
                sceneForwardPass.drawGrass(commandBuffer, sceneDescriptorPass.grassDescriptorSet(currentFrame),
                                           grassDraw);
            }
            sceneSkyPass.record(commandBuffer, currentFrame);
            if (particleSystem) {
                Camera sceneCamera{Degrees{60.0F},
                                   static_cast<float>(sceneViewportTarget.extent().width) /
                                       static_cast<float>(sceneViewportTarget.extent().height),
                                   0.1F, 1000.0F};
                sceneCamera.setPosition(cameraController.editorPosition());
                sceneCamera.setRotation(Degrees{cameraController.editorYaw()}, Degrees{cameraController.editorPitch()});
                Particles::ParticleFrameData particleFrame{
                    sceneCamera.projectionMatrix() * sceneCamera.viewMatrix(),
                    sceneCamera.projectionMatrix() * sceneCamera.viewMatrix(),
                    sceneCamera.right(),
                    0.0F,
                    sceneCamera.up(),
                    0.0F,
                    sceneCamera.right(),
                    0.0F,
                    sceneCamera.up(),
                    0.0F,
                    static_cast<float>(Time::deltaTime()),
                };
                particleSystem->recordRender(commandBuffer, particleFrame, sceneParticlePipeline.handle(),
                                             sceneParticlePipeline.layout(), currentFrame, true);
            }
            ForwardPass::end(commandBuffer);
            VkImageMemoryBarrier2 colorToSampled{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
            colorToSampled.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
            colorToSampled.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
            colorToSampled.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
            colorToSampled.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
            colorToSampled.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            colorToSampled.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            colorToSampled.image = sceneViewportTarget.color().image();
            colorToSampled.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            VkImageMemoryBarrier2 depthToSampled{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
            depthToSampled.srcStageMask =
                VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
            depthToSampled.srcAccessMask = 0;
            depthToSampled.dstStageMask =
                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            depthToSampled.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
            // Lighting only tested against the prepass depth (read-only).
            depthToSampled.oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
            depthToSampled.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
            depthToSampled.image = sceneViewportDepthBuffer.image();
            depthToSampled.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
            const VkImageMemoryBarrier2 sceneToSampled[] = {colorToSampled, depthToSampled};
            VkDependencyInfo sceneToSampledDependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            sceneToSampledDependency.imageMemoryBarrierCount = std::size(sceneToSampled);
            sceneToSampledDependency.pImageMemoryBarriers = sceneToSampled;
            vkCmdPipelineBarrier2(commandBuffer, &sceneToSampledDependency);
            sceneViewportDepthInitialized = true;
            gpuTimestampProfiler.endZone(commandBuffer, currentFrame);
            recordVsmPageRequests(commandBuffer, true);

            const bool hasSceneLegacyWater = activeShaderSlots.test(materialShaderIndex(MaterialShader::Water));
            const bool hasSceneVirtualWater = sceneVirtualWaterRenderer.active();
            if (hasSceneLegacyWater || hasSceneVirtualWater) {
                const VkImageMemoryBarrier2 beforeCopy[] = {
                    {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                     nullptr,
                     VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                     VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                     VK_ACCESS_2_TRANSFER_READ_BIT,
                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     VK_QUEUE_FAMILY_IGNORED,
                     VK_QUEUE_FAMILY_IGNORED,
                     sceneViewportTarget.color().image(),
                     {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}},
                    {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                     nullptr,
                     sceneOpaqueColorInitialized ? VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT : VK_PIPELINE_STAGE_2_NONE,
                     sceneOpaqueColorInitialized ? VK_ACCESS_2_SHADER_SAMPLED_READ_BIT : 0,
                     VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                     VK_ACCESS_2_TRANSFER_WRITE_BIT,
                     sceneOpaqueColorInitialized ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_QUEUE_FAMILY_IGNORED,
                     VK_QUEUE_FAMILY_IGNORED,
                     sceneOpaqueColor.image(),
                     {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}},
                };
                VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
                dependency.imageMemoryBarrierCount = std::size(beforeCopy);
                dependency.pImageMemoryBarriers = beforeCopy;
                vkCmdPipelineBarrier2(commandBuffer, &dependency);
                const VkExtent2D extent = sceneViewportTarget.extent();
                const VkImageCopy copy{{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                       {0, 0, 0},
                                       {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                       {0, 0, 0},
                                       {extent.width, extent.height, 1}};
                vkCmdCopyImage(commandBuffer, sceneViewportTarget.color().image(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               sceneOpaqueColor.image(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
                const VkImageMemoryBarrier2 afterCopy[] = {
                    {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                     nullptr,
                     VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                     VK_ACCESS_2_TRANSFER_READ_BIT,
                     VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                     VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     // Virtual Water composites into the target as a color attachment.
                     hasSceneVirtualWater ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
                                          : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                     VK_QUEUE_FAMILY_IGNORED,
                     VK_QUEUE_FAMILY_IGNORED,
                     sceneViewportTarget.color().image(),
                     {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}},
                    {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                     nullptr,
                     VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                     VK_ACCESS_2_TRANSFER_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                     VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                     VK_QUEUE_FAMILY_IGNORED,
                     VK_QUEUE_FAMILY_IGNORED,
                     sceneOpaqueColor.image(),
                     {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}},
                };
                dependency.imageMemoryBarrierCount = std::size(afterCopy);
                dependency.pImageMemoryBarriers = afterCopy;
                vkCmdPipelineBarrier2(commandBuffer, &dependency);
                sceneOpaqueColorInitialized = true;

                const auto commandOffset = binnedCommandOffset(static_cast<std::uint32_t>(materialShaderIndex(MaterialShader::Water)), false);
                const auto countOffset =
                    static_cast<VkDeviceSize>(materialShaderIndex(MaterialShader::Water)) * sizeof(std::uint32_t);
                if (hasSceneLegacyWater || hasSceneVirtualWater) {
                    static const ProfileNameId sceneWaterPrepassProfileName =
                        Profiler::registerName("Scene Virtual Water Prepass");
                    static const ProfileNameId sceneWaterShadeProfileName =
                        Profiler::registerName("Scene Virtual Water Shading");
                    gpuTimestampProfiler.beginZone(commandBuffer, currentFrame, sceneWaterPrepassProfileName);
                    sceneVirtualWaterRenderer.recordPrepass(
                        commandBuffer, currentFrame, sceneDescriptorPass.descriptorSet(currentFrame),
                        vertexBuffer.handle(), indexBuffer.handle(), sceneIndirectDraws[currentFrame], commandOffset,
                        countOffset);
                    gpuTimestampProfiler.endZone(commandBuffer, currentFrame);
                    gpuTimestampProfiler.beginZone(commandBuffer, currentFrame, sceneWaterShadeProfileName);
                    sceneVirtualWaterRenderer.recordAdaptiveShading(commandBuffer, currentFrame,
                                                                    sceneDescriptorPass.descriptorSet(currentFrame));
                    sceneVirtualWaterRenderer.recordComposite(commandBuffer, currentFrame,
                                                              sceneDescriptorPass.descriptorSet(currentFrame));
                    gpuTimestampProfiler.endZone(commandBuffer, currentFrame);
                }
                if (hasSceneVirtualWater) {
                    VkImageMemoryBarrier2 toSampled{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
                    toSampled.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
                    toSampled.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
                    toSampled.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
                    toSampled.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
                    toSampled.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                    toSampled.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                    toSampled.srcQueueFamilyIndex = toSampled.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    toSampled.image = sceneViewportTarget.color().image();
                    toSampled.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                    VkDependencyInfo toSampledDependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
                    toSampledDependency.imageMemoryBarrierCount = 1;
                    toSampledDependency.pImageMemoryBarriers = &toSampled;
                    vkCmdPipelineBarrier2(commandBuffer, &toSampledDependency);
                }
            }
            if (activeShaderSlots.test(GlassProgramSlot) && !sceneGlassPass.created() &&
                sceneGlassPassInfo.device != VK_NULL_HANDLE)
                sceneGlassPass.create(sceneGlassPassInfo);
            if (activeShaderSlots.test(GlassProgramSlot) && sceneGlassPass.created() &&
                sceneGlassPass.extent().width == sceneViewportTarget.extent().width &&
                sceneGlassPass.extent().height == sceneViewportTarget.extent().height) {
                static const ProfileNameId sceneGlassProfileName = Profiler::registerName("Scene Glass");
                gpuTimestampProfiler.beginZone(commandBuffer, currentFrame, sceneGlassProfileName);
                sceneGlassPass.record(
                    commandBuffer,
                    {.color = sceneViewportTarget.color().image(),
                     .colorView = sceneViewportTarget.color().imageView(),
                     .depth = sceneViewportDepthBuffer.image(),
                     .sceneDescriptorSet = sceneDescriptorPass.descriptorSet(currentFrame),
                     .vertexBuffer = vertexBuffer.handle(),
                     .indexBuffer = indexBuffer.handle(),
                     .singleSided = &sceneIndirectDraws[currentFrame],
                     .twoSided = &sceneFoliageIndirectDraws[currentFrame],
                     .commandOffset = binnedCommandOffset(static_cast<std::uint32_t>(GlassProgramSlot), false),
                     .twoSidedCommandOffset = binnedCommandOffset(static_cast<std::uint32_t>(GlassProgramSlot), true),
                     .countOffset = static_cast<VkDeviceSize>(GlassProgramSlot) * sizeof(std::uint32_t)});
                gpuTimestampProfiler.endZone(commandBuffer, currentFrame);
            }
        }
    }

    // The finished opaque and water lighting is the radiance the next
    // frame's SSGI trace gathers.
    if (ssgiTracedThisFrame) {
        gpuTimestampProfiler.beginZone(commandBuffer, currentFrame, ssgiCaptureProfileName);
        ssgiPass.captureRadiance(commandBuffer, currentFrame, hdrBuffer.imageView(), hdrBuffer.sampler());
        gpuTimestampProfiler.endZone(commandBuffer, currentFrame);
    }

    // Debug readback of the lit HDR scene (before TAA), see Renderer::requestHdrCapture.
    // While only the editor's Scene View renders, its colour target instead.
    const bool captureSceneView = !renderGameViewport && renderSceneViewport;
    if (hdrCaptureRequested && (renderGameViewport || captureSceneView)) {
        const VkExtent2D extent = captureSceneView ? sceneViewportTarget.extent() : swapchain.extent();
        const VkImage captureImage = captureSceneView ? sceneViewportTarget.color().image() : hdrBuffer.image();
        const VkDeviceSize bytes = VkDeviceSize{extent.width} * extent.height * 8;
        Buffer& capture = hdrCaptureBuffers[currentFrame];
        if (capture.size() < bytes) {
            capture.destroy();
            capture.createHostVisible(vulkanDevice.physical(), device, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                      vulkanDevice.allocator());
        }
        VkImageMemoryBarrier2 toCopy{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        toCopy.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                              VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        toCopy.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_SHADER_WRITE_BIT;
        toCopy.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        toCopy.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
        toCopy.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        toCopy.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        toCopy.srcQueueFamilyIndex = toCopy.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toCopy.image = captureImage;
        toCopy.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dependency.imageMemoryBarrierCount = 1;
        dependency.pImageMemoryBarriers = &toCopy;
        vkCmdPipelineBarrier2(commandBuffer, &dependency);
        const VkBufferImageCopy region{0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0},
                                       {extent.width, extent.height, 1}};
        vkCmdCopyImageToBuffer(commandBuffer, captureImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               capture.handle(), 1, &region);
        VkImageMemoryBarrier2 back = toCopy;
        back.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        back.srcAccessMask = 0;
        back.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        back.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        back.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        back.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        dependency.pImageMemoryBarriers = &back;
        vkCmdPipelineBarrier2(commandBuffer, &dependency);
        hdrCaptureExtent = extent;
        hdrCapturePending[currentFrame] = true;
        hdrCaptureRequested = false;
    }

    // Initialize before importing: from here the render graph is the sole
    // owner of the color-history and bloom image layouts.
    if (renderGameViewport && taaResolveActive)
        temporalAaPass.prepareHistory(commandBuffer);
    bloomPass.prepare(commandBuffer);
    // Everything before presentation goes to the GPU first; only then does
    // the CPU wait for a swapchain image. Acquiring at the start of the frame
    // blocked the CPU ~2.4 ms per frame before it had recorded anything, so
    // the GPU idled ~0.7 ms of every frame waiting for work (Nsight Systems).
    if (vkEndCommandBuffer(commandBuffer) != VK_SUCCESS)
        throw std::runtime_error("Could not end graphics commands before presentation RenderGraph");
    const std::uint64_t prePresentationUploadCandidate = legacyUploadValue();
    const std::uint64_t prePresentationUploadValue =
        prePresentationUploadCandidate != 0 && uploadContext.completedValue() < prePresentationUploadCandidate
            ? prePresentationUploadCandidate
            : 0;
    const VkSemaphoreSubmitInfo prePresentationUploadWait{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
                                                          .semaphore = uploadContext.timeline(),
                                                          .value = prePresentationUploadValue,
                                                          .stageMask = legacyUploadStages};
    const VkCommandBufferSubmitInfo prePresentationCommand{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
                                                           .commandBuffer = commandBuffer};
    const VkSubmitInfo2 prePresentationSubmit{
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
        .waitSemaphoreInfoCount = prePresentationUploadValue != 0 ? 1U : 0U,
        .pWaitSemaphoreInfos = prePresentationUploadValue != 0 ? &prePresentationUploadWait : nullptr,
        .commandBufferInfoCount = 1,
        .pCommandBufferInfos = &prePresentationCommand};
    if (vkQueueSubmit2(vulkanDevice.graphicsQueue(), 1, &prePresentationSubmit, VK_NULL_HANDLE) != VK_SUCCESS)
        throw std::runtime_error("Could not submit graphics commands before presentation RenderGraph");
    if (!acquireSwapchainImage())
        return;
    const SwapchainImageIndex imageIndex{acquiredImageIndex};

    // Presentation is one declarative chain. The graph owns the HDR
    // history and swapchain transitions between TAA, bloom and raster passes.
    frameGraph.setQueueFamily(RenderGraph::Queue::Graphics, vulkanDevice.graphicsQueueFamily());
    const VkExtent2D postExtent = swapchain.extent();
    const RenderGraph::TextureDesc hdrDesc{.extent = {postExtent.width, postExtent.height, 1},
                                           .format = HdrBuffer::Format,
                                           .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                                           .aspect = VK_IMAGE_ASPECT_COLOR_BIT};
    auto taaHistoryDesc = hdrDesc;
    taaHistoryDesc.usage |= VK_IMAGE_USAGE_STORAGE_BIT;
    const RenderGraph::TextureDesc velocityDesc{.extent = {postExtent.width, postExtent.height, 1},
                                                .format = VK_FORMAT_R16G16_SFLOAT,
                                                .usage =
                                                    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                                                .aspect = VK_IMAGE_ASPECT_COLOR_BIT};
    const auto graphHdr = frameGraph.importTexture("Game HDR", hdrBuffer.image(), hdrDesc,
                                                   {.stage = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                                                    .access = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                                                    .layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
    const auto graphVelocity = frameGraph.importTexture("Game velocity", velocityBuffer.image(), velocityDesc,
                                                        {.stage = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                                                         .access = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                                                         .layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
    const DepthBuffer& taaDepth = msaa.enabled() ? hiZDepthBuffer : depthBuffer;
    const RenderGraph::TextureDesc depthDesc{.extent = {postExtent.width, postExtent.height, 1},
                                             .format = taaDepth.format(),
                                             .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                                                      VK_IMAGE_USAGE_SAMPLED_BIT,
                                             .aspect = VK_IMAGE_ASPECT_DEPTH_BIT};
    const auto graphDepth = frameGraph.importTexture("Game depth", taaDepth.image(), depthDesc,
                                                     {.stage = VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                                                      .access = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                                                      .layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
                                                      .write = true});
    const auto graphBloom = frameGraph.importTexture("Bloom", bloomPass.resultImage(), hdrDesc,
                                                     {.stage = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                                                      .access = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                                                      .layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
    const RenderGraph::TextureDesc presentDesc{.extent = {postExtent.width, postExtent.height, 1},
                                               .format = swapchain.format(),
                                               .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                                               .aspect = VK_IMAGE_ASPECT_COLOR_BIT};
    // The acquire semaphore is waited on at COLOR_ATTACHMENT_OUTPUT; the
    // graph's first transition must start from that stage to chain with it
    // (the presentation engine may still read the image until then).
    const auto graphPresent =
        frameGraph.importTexture("Swapchain", swapchain.images().at(imageIndex.value), presentDesc,
                                 {.stage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                                  .layout = VK_IMAGE_LAYOUT_UNDEFINED});

    const Mat4 taaProjection = cameraController.camera()->projectionMatrix();
    RenderGraph::TextureHandle graphPostSource = graphHdr;
    if (renderGameViewport && taaResolveActive) {
        const std::uint32_t historyReadIndex = temporalAaPass.resolvedIndex();
        const std::uint32_t historyWriteIndex = temporalAaPass.nextResolvedIndex();
        const auto graphHistoryRead =
            frameGraph.importTexture("TAA history read", temporalAaPass.historyImage(historyReadIndex), taaHistoryDesc,
                                     {.stage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                      .access = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                                      .layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
        const auto graphHistoryWrite = frameGraph.importTexture(
            "TAA history write", temporalAaPass.historyImage(historyWriteIndex), taaHistoryDesc,
            {.stage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
             .access = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
             .layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
        RenderGraph::TextureHandle graphWaterVelocity;
        RenderGraph::TextureHandle graphWaterMeta;
        RenderGraph::TextureHandle graphWaterSurface;
        if (virtualWaterPreparedThisFrame) {
            graphWaterVelocity =
                frameGraph.importTexture("Water velocity", virtualWaterRenderer.velocityImage(), hdrDesc,
                                         {.stage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                                          .access = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                                          .layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                          .write = true});
            graphWaterMeta = frameGraph.importTexture("Water metadata", virtualWaterRenderer.metaImage(), hdrDesc,
                                                      {.stage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                                                       .access = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                                                       .layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                                       .write = true});
            graphWaterSurface = frameGraph.importTexture("Water surface", virtualWaterRenderer.surfaceImage(), hdrDesc,
                                                         {.stage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                                                          .access = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                                                          .layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                                          .write = true});
        }
        frameGraph.addPass(
            "TAA resolve", RenderGraph::Queue::Graphics,
            [&](RenderGraph::PassBuilder& builder) {
                builder.read(graphHdr, RenderGraph::TextureUsage::SampledReadCompute);
                builder.read(graphVelocity, RenderGraph::TextureUsage::SampledReadCompute);
                builder.read(graphDepth, RenderGraph::TextureUsage::DepthReadCompute);
                builder.read(graphHistoryRead, RenderGraph::TextureUsage::SampledReadCompute);
                if (graphWaterVelocity) {
                    builder.read(graphWaterVelocity, RenderGraph::TextureUsage::SampledReadCompute);
                    builder.read(graphWaterMeta, RenderGraph::TextureUsage::SampledReadCompute);
                    builder.read(graphWaterSurface, RenderGraph::TextureUsage::SampledReadCompute);
                }
                builder.write(graphHistoryWrite, RenderGraph::TextureUsage::StorageWriteCompute);
                builder.setFinalTextureState(graphHistoryWrite, {.stage = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                                                                 .access = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                                                                 .layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                                                 .write = false});
            },
            [&](const VkCommandBuffer buffer) {
                gpuTimestampProfiler.beginZone(buffer, currentFrame, taaProfileName);
                temporalAaPass.setVirtualWaterEnabled(virtualWaterPreparedThisFrame);
                temporalAaPass.record(buffer, postExtent, taaJitterX, taaJitterY, taaProjection.native()[2][2],
                                      taaProjection.native()[3][2]);
                gpuTimestampProfiler.endZone(buffer, currentFrame);
            });
        graphPostSource = graphHistoryWrite;
    } else {
        frameGraph.addPass(
            "TAA disabled", RenderGraph::Queue::Graphics, [](RenderGraph::PassBuilder&) {},
            [&](const VkCommandBuffer buffer) {
                gpuTimestampProfiler.beginZone(buffer, currentFrame, taaProfileName);
                gpuTimestampProfiler.endZone(buffer, currentFrame);
            });
    }
    if (renderGameViewport) {
        frameGraph.addPass(
            "Bloom", RenderGraph::Queue::Graphics,
            [&](RenderGraph::PassBuilder& builder) {
                builder.read(graphPostSource, RenderGraph::TextureUsage::SampledReadFragment);
                builder.write(graphBloom, RenderGraph::TextureUsage::ColorAttachment);
                builder.setFinalTextureState(graphBloom, {.stage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                                                          .access = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                                                          .layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                                          .write = true});
            },
            [&](const VkCommandBuffer buffer) {
                gpuTimestampProfiler.beginZone(buffer, currentFrame, bloomProfileName);
                bloomPass.record(buffer, taaResolveActive ? temporalAaPass.resolvedView() : hdrBuffer.imageView(),
                                 hdrBuffer.sampler(), currentFrame);
                gpuTimestampProfiler.endZone(buffer, currentFrame);
            });
    }
    const std::uint32_t postSourceIndex = taaResolveActive ? 1U + temporalAaPass.resolvedIndex() : 0U;
    if (renderGameViewport && postProcessSettings.autoExposure) {
        frameGraph.addPass(
            "Auto exposure", RenderGraph::Queue::Graphics,
            [&](RenderGraph::PassBuilder& builder) {
                builder.read(graphPostSource, RenderGraph::TextureUsage::SampledReadCompute);
                // Its output (the adapted luminance) is not a graph resource,
                // so without this the pass would be culled as dead.
                builder.setSideEffect();
            },
            [&](const VkCommandBuffer buffer) {
                tonemapPass.recordExposure(buffer, postProcessSettings, static_cast<float>(Time::deltaTime()),
                                           postSourceIndex);
            });
    }
    // Motion blur and moving-pixel sharpening read the velocity target, which
    // is only kept current (and shader-readable) for the TAA resolve.
    const bool postReadsVelocity = renderGameViewport && taaResolveActive && !msaa.enabled();
    frameGraph.addPass(
        editorUiActive ? "Editor UI" : "Tonemap and UI", RenderGraph::Queue::Graphics,
        [&](RenderGraph::PassBuilder& builder) {
            if (editorUiActive) {
                // The editor samples the tonemapped Game View image drawn
                // at the start of this pass, so it reads what the Player's
                // tonemap reads.
                if (renderGameViewport) {
                    builder.read(graphPostSource, RenderGraph::TextureUsage::SampledReadFragment);
                    builder.read(graphBloom, RenderGraph::TextureUsage::SampledReadFragment);
                }
            } else {
                builder.read(graphPostSource, RenderGraph::TextureUsage::SampledReadFragment);
                if (renderGameViewport)
                    builder.read(graphBloom, RenderGraph::TextureUsage::SampledReadFragment);
            }
            if (postReadsVelocity)
                builder.read(graphVelocity, RenderGraph::TextureUsage::SampledReadFragment);
            builder.write(graphPresent, RenderGraph::TextureUsage::ColorAttachment);
            builder.setFinalTextureState(graphPresent, {.stage = VK_PIPELINE_STAGE_2_NONE,
                                                        .access = VK_ACCESS_2_NONE,
                                                        .layout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                                                        .write = true});
        },
        [&](const VkCommandBuffer buffer) {
            gpuTimestampProfiler.beginZone(buffer, currentFrame, tonemapProfileName);
            if (editorUiActive) {
                if (renderGameViewport)
                    tonemapPass.recordDisplay(buffer, postExtent, postProcessSettings, postSourceIndex);
                VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
                color.imageView = swapchain.imageViews().at(imageIndex.value);
                color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
                color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                color.clearValue.color = {{0.06F, 0.07F, 0.09F, 1.0F}};
                VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
                rendering.renderArea.extent = postExtent;
                rendering.layerCount = 1;
                rendering.colorAttachmentCount = 1;
                rendering.pColorAttachments = &color;
                vkCmdBeginRendering(buffer, &rendering);
                if (editorUiBackend)
                    editorUiBackend->renderDrawData(reinterpret_cast<std::uint64_t>(buffer));
                vkCmdEndRendering(buffer);
            } else {
                tonemapPass.record(buffer, imageIndex.value, postExtent, postProcessSettings, postSourceIndex);
                canvasRenderer.record(scene.uiCanvas(), buffer, UI::CanvasRenderer::ImageIndex{imageIndex.value},
                                      UI::CanvasRenderer::FrameIndex{currentFrame}, postExtent);
            }
            gpuTimestampProfiler.endZone(buffer, currentFrame);
            gpuTimestampProfiler.endFrame(buffer, currentFrame);
        });
    // Final-image readback (Renderer::requestFrameCapture): exactly what is
    // presented, after exposure, the display transform, bloom and UI.
    if (frameCaptureRequested && swapchain.supportsCapture()) {
        const VkDeviceSize bytes = VkDeviceSize{postExtent.width} * postExtent.height * 4;
        Buffer& capture = frameCaptureBuffers[currentFrame];
        if (capture.size() < bytes) {
            capture.destroy();
            capture.createHostVisible(vulkanDevice.physical(), device, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                      vulkanDevice.allocator());
        }
        const VkImage presentImage = swapchain.images().at(imageIndex.value);
        frameGraph.addPass(
            "Frame capture", RenderGraph::Queue::Graphics,
            [&](RenderGraph::PassBuilder& builder) {
                builder.read(graphPresent, RenderGraph::TextureUsage::TransferRead);
                // The last user decides the hand-off layout for presentation.
                builder.setFinalTextureState(graphPresent, {.stage = VK_PIPELINE_STAGE_2_NONE,
                                                            .access = VK_ACCESS_2_NONE,
                                                            .layout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                                                            .write = false});
                // The copy lands in a host buffer outside the graph.
                builder.setSideEffect();
            },
            [&, presentImage](const VkCommandBuffer buffer) {
                const VkBufferImageCopy region{0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0},
                                               {postExtent.width, postExtent.height, 1}};
                vkCmdCopyImageToBuffer(buffer, presentImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                       frameCaptureBuffers[currentFrame].handle(), 1, &region);
            });
        frameCaptureExtents[currentFrame] = postExtent;
        frameCapturePending[currentFrame] = true;
        frameCaptureRequested = false;
    }
    frameGraph.exportTexture(graphPresent);
    frameGraph.compile();
    profileRenderGraph(frameGraph, true);
    const auto& presentationBatches = frameGraph.queueBatches();
    std::uint32_t firstGraphicsBatch = 0;
    bool foundGraphicsBatch = false;
    for (std::uint32_t i = 0; i < presentationBatches.size(); ++i) {
        if (presentationBatches[i].queue == RenderGraph::Queue::Graphics) {
            firstGraphicsBatch = i;
            foundGraphicsBatch = true;
            break;
        }
    }
    if (!foundGraphicsBatch)
        throw std::runtime_error("Presentation RenderGraph has no graphics batch");
    RenderGraph::SubmissionContext presentationContext{};
    presentationContext.queues[static_cast<std::uint32_t>(RenderGraph::Queue::Graphics)] = vulkanDevice.graphicsQueue();
    presentationContext.queues[static_cast<std::uint32_t>(RenderGraph::Queue::AsyncCompute)] =
        renderPath == RenderPath::AsyncCompute && vulkanDevice.hasAsyncComputeQueue() ? vulkanDevice.computeQueue()
                                                                                      : vulkanDevice.graphicsQueue();
    presentationContext.queues[static_cast<std::uint32_t>(RenderGraph::Queue::Transfer)] =
        renderPath == RenderPath::AsyncCompute && vulkanDevice.hasAsyncTransferQueue() ? vulkanDevice.transferQueue()
                                                                                       : vulkanDevice.graphicsQueue();
    presentationContext.commandBuffers =
        prepareGraphCommandBuffers(frameGraph, currentFrame, presentationGraphCommandBuffers);
    presentationContext.queueTimelines[0] = renderGraphTimeline;
    presentationContext.queueTimelines[1] = renderGraphComputeTimeline;
    presentationContext.queueTimelines[2] = renderGraphTransferTimeline;
    presentationContext.nextQueueTimelineValues[0] = &renderGraphTimelineValue;
    presentationContext.nextQueueTimelineValues[1] = &renderGraphComputeTimelineValue;
    presentationContext.nextQueueTimelineValues[2] = &renderGraphTransferTimelineValue;
    presentationContext.uploadTimeline = uploadContext.timeline();
    presentationContext.externalWaits.push_back({.semaphore = imageAvailableSemaphores[currentFrame],
                                                 .value = 0,
                                                 .stage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                                                 .batch = firstGraphicsBatch});
    presentationContext.completionSignals.push_back({.semaphore = renderFinishedSemaphores[imageIndex.value],
                                                     .value = 0,
                                                     .stage = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT});
    presentationContext.completionFence = inFlightFences[currentFrame];
    // Join the viewport graph's trailing async work (Hi-Z, GTAO) before the
    // frame fence; the next frame reads the pyramid and rewrites depth.
    const std::array<VkSemaphore, 3> viewportTimelines{renderGraphTimeline, renderGraphComputeTimeline,
                                                      renderGraphTransferTimeline};
    for (std::uint32_t queue = 0; queue < viewportTimelines.size(); ++queue)
        if (viewportGraphCompletion[queue] != 0)
            presentationContext.completionWaits.push_back({.semaphore = viewportTimelines[queue],
                                                           .value = viewportGraphCompletion[queue],
                                                           .stage = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT});
    viewportGraphCompletion = {};
    frameGraph.recordAndSubmit(presentationContext);
    presentationGraphSubmittedThisFrame = true;
}

void createSyncObjects() {
    imageAvailableSemaphores.resize(MAX_FRAMES_IN_FLIGHT);
    renderFinishedSemaphores.resize(swapchain.imageCount());
    inFlightFences.resize(MAX_FRAMES_IN_FLIGHT);

    VkSemaphoreCreateInfo semaphoreInfo{};
    semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

    VkSemaphoreTypeCreateInfo timelineInfo{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
    timelineInfo.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    semaphoreInfo.pNext = &timelineInfo;
    if (vkCreateSemaphore(device, &semaphoreInfo, nullptr, &renderGraphTimeline) != VK_SUCCESS) {
        throw std::runtime_error("Could not create RenderGraph timeline semaphore");
    }
    if (vkCreateSemaphore(device, &semaphoreInfo, nullptr, &renderGraphComputeTimeline) != VK_SUCCESS ||
        vkCreateSemaphore(device, &semaphoreInfo, nullptr, &renderGraphTransferTimeline) != VK_SUCCESS) {
        throw std::runtime_error("Could not create RenderGraph queue timeline semaphores");
    }
    semaphoreInfo.pNext = nullptr;

    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        if (vkCreateSemaphore(device, &semaphoreInfo, nullptr, &imageAvailableSemaphores[i]) != VK_SUCCESS ||
            vkCreateFence(device, &fenceInfo, nullptr, &inFlightFences[i]) != VK_SUCCESS) {
            throw std::runtime_error("Could not create synchronization objects");
        }
    }

    createRenderFinishedSemaphores();
}

// ---------- SWAPCHAIN RECREATE ----------

void cleanupSwapChain() {
    virtualWaterRenderer.destroy();
    destroyEditorUiResources();
    canvasRenderer.destroy();
    tonemapPass.destroy();
    temporalAaPass.destroy();
    bloomPass.destroy();
    gtaoPass.destroy();
    ssgiPass.destroy();
    destroyVelocityResources();
    if (hdrFramebuffer != VK_NULL_HANDLE) {
        vkDestroyFramebuffer(device, hdrFramebuffer, nullptr);
        hdrFramebuffer = VK_NULL_HANDLE;
    }
    if (lightingHdrFramebuffer != VK_NULL_HANDLE) {
        vkDestroyFramebuffer(device, lightingHdrFramebuffer, nullptr);
        lightingHdrFramebuffer = VK_NULL_HANDLE;
    }
    if (waterHdrFramebuffer != VK_NULL_HANDLE) {
        vkDestroyFramebuffer(device, waterHdrFramebuffer, nullptr);
        waterHdrFramebuffer = VK_NULL_HANDLE;
    }
    destroySceneViewportResources();

    msaa.destroy();
    hdrBuffer.destroy();
    hdrBufferInitialized = false;
    opaqueSceneColor.destroy();
    opaqueSceneColorInitialized = false;
    destroyDepthResources();
    destroyRenderFinishedSemaphores();
    swapchain.destroy();
}

void destroyRenderFinishedSemaphores() noexcept {
    for (VkSemaphore semaphore : renderFinishedSemaphores) {
        if (semaphore != VK_NULL_HANDLE) {
            vkDestroySemaphore(device, semaphore, nullptr);
        }
    }
    renderFinishedSemaphores.clear();
}

void createRenderFinishedSemaphores() {
    renderFinishedSemaphores.resize(swapchain.imageCount());

    VkSemaphoreCreateInfo semaphoreInfo{};
    semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    for (VkSemaphore& semaphore : renderFinishedSemaphores) {
        if (vkCreateSemaphore(device, &semaphoreInfo, nullptr, &semaphore) != VK_SUCCESS) {
            destroyRenderFinishedSemaphores();
            throw std::runtime_error("Could not create render-finished semaphore");
        }
    }
}

void recreateSwapChain() {
    waitForDrawableExtent();

    vkDeviceWaitIdle(device);
    sceneViewportCacheValid = false;
    sceneViewportImageInitialized = false;
    sceneViewportNeedsRender = true;

    // The ImGui Vulkan backend owns swapchain-dependent render data.
    // Tear down both ImGui backends before recreating the presentation
    // resources, then register the SDL window again below.
    destroyEditorUiResources();

    // Only extent-dependent resources are rebuilt. The VSM passes, their
    // physical page cache, DDGI and the cluster ray-tracing scene do not
    // depend on the swapchain size: shadowPass and sceneDescriptorPass keep
    // their pipelines and layouts, and createShadowPass() /
    // createSceneDescriptorPass() below take their updateDescriptors() path
    // to rebind the recreated culling buffers and GTAO/SSGI targets (the
    // same path as a topology-only culling rebuild). The DDGI trace layout
    // embeds shadowPass's set layout, which survives, so DDGI survives too.
    virtualWaterRenderer.destroy();
    forwardPass.destroy();
    lightingForwardPass.destroy();
    waterPass.destroy();
    glassPass.destroy();
    directionalVisibilityPass.destroy();
    destroyCullingResources();

    canvasRenderer.destroy();
    tonemapPass.destroy();
    temporalAaPass.destroy();
    bloomPass.destroy();
    gtaoPass.destroy();
    ssgiPass.destroy();
    destroyVelocityResources();
    if (hdrFramebuffer != VK_NULL_HANDLE) {
        vkDestroyFramebuffer(device, hdrFramebuffer, nullptr);
        hdrFramebuffer = VK_NULL_HANDLE;
    }
    if (lightingHdrFramebuffer != VK_NULL_HANDLE) {
        vkDestroyFramebuffer(device, lightingHdrFramebuffer, nullptr);
        lightingHdrFramebuffer = VK_NULL_HANDLE;
    }
    if (waterHdrFramebuffer != VK_NULL_HANDLE) {
        vkDestroyFramebuffer(device, waterHdrFramebuffer, nullptr);
        waterHdrFramebuffer = VK_NULL_HANDLE;
    }
    destroySceneViewportResources();
    msaa.destroy();
    hdrBuffer.destroy();
    hdrBufferInitialized = false;
    opaqueSceneColor.destroy();
    opaqueSceneColorInitialized = false;
    destroyDepthResources();
    destroyRenderFinishedSemaphores();

    swapchain.recreate();
    registry.view<CameraComponent>([&](const Entity, CameraComponent& component) {
        if (component.primary) {
            component.setAspectRatio(static_cast<float>(swapchain.extent().width),
                                     static_cast<float>(swapchain.extent().height));
        }
    });
    if (!sceneResourcesInitialized) {
        createRenderFinishedSemaphores();
        createEditorUiResources(false);
        return;
    }
    hdrBuffer.create(vulkanDevice.physical(), device, swapchain.extent(), vulkanDevice.allocator(),
                     VK_FILTER_LINEAR, HdrBuffer::Format, true); // storage: deferred local lights
    hdrBufferInitialized = false;
    opaqueSceneColor.create(vulkanDevice.physical(), device, swapchain.extent(), vulkanDevice.allocator());
    opaqueSceneColorInitialized = false;
    msaa.create(swapchain.extent(), HdrBuffer::Format);
    createDepthResources();
    createGtaoPass();
    createSsgiPass();
    createRenderFinishedSemaphores();

    createCullingResources();
    createShadowPass();
    createSceneDescriptorPass();
    createForwardPass();
    createFramebuffers();
    createSceneViewportResources();
    createTemporalAaPass();
    createBloomPass();
    createTonemapPass();
    createUIResources();
    createEditorUiResources();
}

// ---------- MAIN LOOP ----------

void updateCameraInput() {
    cameraController.update(registry);
}

void updateEditorSceneCameraInput() {
    if (!cameraController.editorInputEnabled())
        return;
    const Vec3 beforePosition = cameraController.editorPosition();
    const float beforeYaw = cameraController.editorYaw();
    const float beforePitch = cameraController.editorPitch();
    cameraController.updateEditor();
    if (beforePosition.x() != cameraController.editorPosition().x() ||
        beforePosition.y() != cameraController.editorPosition().y() ||
        beforePosition.z() != cameraController.editorPosition().z() || beforeYaw != cameraController.editorYaw() ||
        beforePitch != cameraController.editorPitch()) {
        sceneViewportNeedsRender = true;
    }
}

void rebuildParticleColliderCache() {
    cachedParticleColliders.clear();
    particleColliderEntities.clear();
    particleColliderIndices.clear();
    cachedParticleColliders.reserve(registry.size());
    particleColliderEntities.reserve(registry.size());
    particleColliderIndices.reserve(registry.size());
    const Registry& readRegistry = registry;
    readRegistry.view<ColliderComponent, Transform>(
        [&](const Entity entity, const ColliderComponent& collider, const Transform& transform) {
            particleColliderIndices.emplace(entity, cachedParticleColliders.size());
            cachedParticleColliders.push_back(RendererSceneHelpers::makeParticleCollider(collider, transform));
            particleColliderEntities.push_back(entity);
        });
}

void removeParticleCollider(const Entity entity) {
    const auto colliderIterator = particleColliderIndices.find(entity);
    if (colliderIterator == particleColliderIndices.end()) {
        return;
    }
    const std::size_t index = colliderIterator->second;
    const std::size_t last = cachedParticleColliders.size() - 1;
    if (index != last) {
        cachedParticleColliders[index] = cachedParticleColliders[last];
        const Entity movedEntity = particleColliderEntities[last];
        particleColliderEntities[index] = movedEntity;
        particleColliderIndices[movedEntity] = index;
    }
    cachedParticleColliders.pop_back();
    particleColliderEntities.pop_back();
    particleColliderIndices.erase(colliderIterator);
}

[[nodiscard]] bool synchronizeParticleColliders() {
    const std::uint64_t structuralRevision = registry.structuralRevision();
    const std::uint64_t colliderRevision = registry.componentRevision<ColliderComponent>();
    const std::uint64_t transformRevision = registry.componentRevision<Transform>();
    const bool rebuild =
        particleColliderRegistry != &registry || particleColliderStructuralRevision != structuralRevision;
    bool cacheChanged = false;

    if (rebuild) {
        rebuildParticleColliderCache();
        cacheChanged = true;
    } else if (particleColliderComponentRevision != colliderRevision ||
               particleColliderTransformRevision != transformRevision) {
        // Refreshing an entity is idempotent, so an entity reported by both
        // logs may simply be visited twice. This used to collect both logs
        // into fresh vectors and an unordered_set on every frame in which
        // anything moved.
        const Registry& readRegistry = registry;
        const auto refresh = [&](const Entity entity) {
            const auto* collider = readRegistry.tryGet<ColliderComponent>(entity);
            const auto* transform = collider != nullptr ? readRegistry.tryGet<Transform>(entity) : nullptr;
            const auto colliderIterator = particleColliderIndices.find(entity);
            if (transform == nullptr) {
                if (colliderIterator != particleColliderIndices.end()) {
                    removeParticleCollider(entity);
                    cacheChanged = true;
                }
                return;
            }
            const Particles::ParticleCollider value = RendererSceneHelpers::makeParticleCollider(*collider, *transform);
            if (colliderIterator != particleColliderIndices.end()) {
                cachedParticleColliders[colliderIterator->second] = value;
            } else {
                particleColliderIndices.emplace(entity, cachedParticleColliders.size());
                cachedParticleColliders.push_back(value);
                particleColliderEntities.push_back(entity);
            }
            cacheChanged = true;
        };
        readRegistry.forEachComponentChangedSinceUnique<ColliderComponent>(particleColliderComponentRevision, refresh);
        readRegistry.forEachComponentChangedSinceUnique<Transform>(particleColliderTransformRevision, refresh);
    }

    particleColliderRegistry = &registry;
    particleColliderStructuralRevision = structuralRevision;
    particleColliderComponentRevision = colliderRevision;
    particleColliderTransformRevision = transformRevision;
    return cacheChanged;
}

// Retires this frame slot's previous submission. The swapchain image is
// acquired separately, as late as possible (acquireSwapchainImage).
void waitFrameSlot() {
    vkWaitForFences(device, 1, &inFlightFences[currentFrame], VK_TRUE, UINT64_MAX);
    swapchainImageAcquired = false;
    if (hdrCapturePending[currentFrame]) {
        hdrCapturePixels.resize(std::size_t{hdrCaptureExtent.width} * hdrCaptureExtent.height * 4);
        hdrCaptureBuffers[currentFrame].read(hdrCapturePixels.data(), hdrCapturePixels.size() * sizeof(std::uint16_t));
        hdrCapturePending[currentFrame] = false;
        hdrCaptureReady = true;
    }
    if (frameCapturePending[currentFrame]) {
        frameCaptureExtent = frameCaptureExtents[currentFrame];
        frameCapturePixels.resize(std::size_t{frameCaptureExtent.width} * frameCaptureExtent.height * 4);
        frameCaptureBuffers[currentFrame].read(frameCapturePixels.data(), frameCapturePixels.size());
        // The swapchain stores BGRA; hand out RGBA.
        if (swapchain.format() == VK_FORMAT_B8G8R8A8_SRGB || swapchain.format() == VK_FORMAT_B8G8R8A8_UNORM)
            for (std::size_t pixel = 0; pixel < frameCapturePixels.size(); pixel += 4)
                std::swap(frameCapturePixels[pixel], frameCapturePixels[pixel + 2]);
        frameCapturePending[currentFrame] = false;
        frameCaptureReady = true;
    }
    // Every buffer retired into this slot belonged to the submission
    // guarded by this fence, so destruction is now safe.
    deferredPreviousTransformBuffers[currentFrame].clear();
    completedFrameValue = std::max(completedFrameValue, frameSubmissionValues[currentFrame]);
    gpuRetirementQueue.collect(completedFrameValue);
    sceneGpu.database.reclaimDeferredInstances(completedFrameValue);
    if (const auto completed = gpuTimestampProfiler.completedFrame(currentFrame)) {
        lastGpuProfile = *completed;
        if (shadowTraceSampled[currentFrame]) {
            shadowTraceSampled[currentFrame] = false;
            for (const GpuProfileEvent& event : completed->events) {
                if (Profiler::name(event.name) != "Local Light Shadows")
                    continue;
                shadowTraceSamples[shadowTraceUsedCluster[currentFrame] ? 1 : 0].push_back(
                    std::max(event.endMs - event.startMs, 0.0F));
                break;
            }
            constexpr std::size_t SamplesPerBackend = 40;
            if (shadowTraceBackend == ShadowTraceBackend::Measuring &&
                shadowTraceSamples[0].size() >= SamplesPerBackend && shadowTraceSamples[1].size() >= SamplesPerBackend) {
                const auto median = [](std::vector<float> values) {
                    std::nth_element(values.begin(), values.begin() + values.size() / 2, values.end());
                    return values[values.size() / 2];
                };
                const float classic = median(shadowTraceSamples[0]);
                const float cluster = median(shadowTraceSamples[1]);
                // The cluster scene must win clearly: it also keeps a second
                // ray tracing scene resident, which a 4 GB GPU feels.
                shadowTraceBackend =
                    cluster <= classic * 0.9F ? ShadowTraceBackend::Cluster : ShadowTraceBackend::Classic;
                saveShadowTraceDecision(classic, cluster);
                // Keep only the winner resident: on a 4 GB GPU the loser's
                // structures and build inputs evicted other render targets.
                if (shadowTraceBackend == ShadowTraceBackend::Classic && !ddgiSettings.enabled) {
                    clusterScene.releaseScene();
                    clusterGeometryDirty = true;
                } else if (shadowTraceBackend == ShadowTraceBackend::Cluster &&
                           rtContactShadowSettings.mode != ContactShadowMode::RayTraced) {
                    accelerationStructures.releaseStructures();
                    rayTracingBlasDirty = vulkanDevice.supportsRayQuery();
                }
                Diagnostics::instance().report(
                    DiagnosticSeverity::Info,
                    "[RayTracing] Local light shadows: cluster scene " + std::to_string(cluster) +
                        " ms, triangle TLAS " + std::to_string(classic) + " ms; using " +
                        (shadowTraceBackend == ShadowTraceBackend::Cluster ? "the cluster scene" : "the triangle TLAS"),
                    {.subsystem = "Renderer"});
            }
        }
        float gameWaterMs = 0.0F;
        float sceneWaterMs = 0.0F;
        if (gameVsmProfiled[currentFrame]) {
            for (const GpuProfileEvent& event : completed->events) {
                if (Profiler::name(event.name) != "Shadow")
                    continue;
                const float gpuMs = std::max(event.endMs - event.startMs, 0.0F);
                smoothedGameVsmMs = smoothedGameVsmMs == 0.0F ? gpuMs : 0.75F * smoothedGameVsmMs + 0.25F * gpuMs;
                // Dense geometry makes every page expensive. A 16-page floor
                // then took dozens of frames to cover a view, showing coarse
                // fallback shadows the whole time; keep refreshing steadily.
                constexpr float targetVsmMs = 3.0F;
                if (smoothedGameVsmMs > targetVsmMs)
                    adaptiveShadowPageBudget = std::max(32u, adaptiveShadowPageBudget - 8u);
                else if (smoothedGameVsmMs < targetVsmMs * 0.7F)
                    adaptiveShadowPageBudget = std::min(128u, adaptiveShadowPageBudget + 4u);
                break;
            }
        }
        for (const GpuProfileEvent& event : completed->events) {
            const std::string_view name = Profiler::name(event.name);
            const float duration = std::max(event.endMs - event.startMs, 0.0F);
            if (name == "Water Page Cull" || name == "Virtual Water Prepass" || name == "Virtual Water Shading")
                gameWaterMs += duration;
            else if (name == "Scene Water Page Cull" || name == "Scene Virtual Water Prepass" ||
                     name == "Scene Virtual Water Shading")
                sceneWaterMs += duration;
        }
        virtualWaterRenderer.onFrameCompleted(currentFrame, gameWaterMs);
        sceneVirtualWaterRenderer.onFrameCompleted(currentFrame, sceneWaterMs);
    } else {
        virtualWaterRenderer.onFrameCompleted();
        sceneVirtualWaterRenderer.onFrameCompleted();
    }
    gameVsmProfiled[currentFrame] = false;
}

// On OUT_OF_DATE the caller recreates the swapchain once the frame's
// recording has unwound; the fence stays signalled, as nothing will signal it.
[[nodiscard]] bool acquireSwapchainImage() {
    if (swapchainImageAcquired)
        return true;
    const VkResult result = vkAcquireNextImageKHR(device, swapchain.handle(), UINT64_MAX,
                                                  imageAvailableSemaphores[currentFrame], VK_NULL_HANDLE,
                                                  &acquiredImageIndex);
    if (result == VK_ERROR_OUT_OF_DATE_KHR)
        return false;
    if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
        throw std::runtime_error("Could not acquire swap chain image");
    }
    // Reset only once this frame is certain to submit the fence's signal.
    vkResetFences(device, 1, &inFlightFences[currentFrame]);
    swapchainImageAcquired = true;
    return true;
}

[[nodiscard]] bool acquireFrameImage(uint32_t& imageIndex) {
    waitFrameSlot();
    if (!acquireSwapchainImage()) {
        recreateSwapChain();
        return false;
    }
    imageIndex = acquiredImageIndex;
    return true;
}

void updateParticleSystemForFrame() {
    if (!particleSystem) {
        return;
    }

    const Entity particleEntity = scene.particleEntity();
    const bool hasEmitter = particleEntity != NullEntity && (registry.has<ParticleEmitterComponent>(particleEntity) ||
                                                             registry.has<SmokeEmitterComponent>(particleEntity));
    if (hasEmitter) {
        if (registry.has<SmokeEmitterComponent>(particleEntity)) {
            auto emitter = registry.get<SmokeEmitterComponent>(particleEntity).emitter;
            if (registry.has<Transform>(particleEntity)) {
                emitter.position = registry.get<Transform>(particleEntity).position;
            }
            particleSystem->setEmitter(emitter);
        } else {
            auto emitter = registry.get<ParticleEmitterComponent>(particleEntity).emitter;
            if (registry.has<Transform>(particleEntity)) {
                emitter.position = registry.get<Transform>(particleEntity).position;
            }
            particleSystem->setEmitter(emitter);
        }
    }
    if (synchronizeParticleColliders()) {
        particleSystem->setColliders(cachedParticleColliders);
    }
    particleSystem->update(static_cast<float>(Time::deltaTime()));
}

void submitAndPresentFrame(const uint32_t imageIndex) {
    if (!presentationGraphSubmittedThisFrame) {
        // Do not derive this from UploadContext::lastSubmittedValue(): a
        // transfer unrelated to this frame must not stall graphics.  The
        // authoritative frame graph supplies only imported resources that
        // have an actual consumer in this frame.
        std::uint64_t uploadValue = 0;
        VkPipelineStageFlags2 uploadStage = VK_PIPELINE_STAGE_2_NONE;
        for (const RenderGraph::UploadWait& wait : frameGraph.uploadWaits()) {
            if (wait.queue != RenderGraph::Queue::Graphics)
                continue;
            uploadValue = std::max(uploadValue, wait.timelineValue);
            uploadStage |= wait.stage;
        }
        uploadValue = uploadValue != 0 && uploadContext.completedValue() < uploadValue ? uploadValue : 0;
        const bool waitForUploads = uploadValue != 0;
        if (uploadStage == VK_PIPELINE_STAGE_2_NONE)
            uploadStage = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        const std::array<VkSemaphoreSubmitInfo, 2> waits{
            VkSemaphoreSubmitInfo{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
                                  .semaphore = imageAvailableSemaphores[currentFrame],
                                  .stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT},
            VkSemaphoreSubmitInfo{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
                                  .semaphore = uploadContext.timeline(),
                                  .value = uploadValue,
                                  .stageMask = uploadStage}};
        const VkCommandBufferSubmitInfo command{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
                                                .commandBuffer = commandBuffers[currentFrame]};
        const VkSemaphoreSubmitInfo signal{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
                                           .semaphore = renderFinishedSemaphores[imageIndex],
                                           .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT};
        const VkSubmitInfo2 submitInfo{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
                                       .waitSemaphoreInfoCount = waitForUploads ? 2u : 1u,
                                       .pWaitSemaphoreInfos = waits.data(),
                                       .commandBufferInfoCount = 1,
                                       .pCommandBufferInfos = &command,
                                       .signalSemaphoreInfoCount = 1,
                                       .pSignalSemaphoreInfos = &signal};
        const VkResult submitResult =
            vkQueueSubmit2(vulkanDevice.graphicsQueue(), 1, &submitInfo, inFlightFences[currentFrame]);
        if (submitResult != VK_SUCCESS) {
            throw std::runtime_error("Could not submit command buffer to queue (VkResult " +
                                     std::to_string(static_cast<int>(submitResult)) + ")");
        }
    }
    frameSubmissionValues[currentFrame] = ++submittedFrameValue;
    VkPresentInfoKHR presentInfo{};
    presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores = &renderFinishedSemaphores[imageIndex];

    VkSwapchainKHR swapChains[] = {swapchain.handle()};
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = swapChains;
    presentInfo.pImageIndices = &imageIndex;

    const VkResult result = vkQueuePresentKHR(vulkanDevice.presentQueue(), &presentInfo);

    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR || framebufferResized) {
        framebufferResized = false;
        recreateSwapChain();
    } else if (result != VK_SUCCESS) {
        throw std::runtime_error("Could not present image");
    }
}

void drawFrame() {
    // A minimized window has no presentable Vulkan extent.  Do not
    // acquire or recreate resources until it becomes drawable again.
    if (!hasDrawableExtent()) {
        return;
    }
    // Frame uploads update stable instances, spatial membership and
    // culling bounds; a cell transition does not rebuild scene tables.
    TransformSystem::updateDirty(registry);
    // refreshSceneFrameData() only rebuilds CPU-side caches. Running it
    // before the in-flight fence wait overlaps it with the GPU still
    // finishing this slot's previous submission.
    {
        GE_PROFILE_SCOPE("Refresh Scene Data");
        refreshSceneFrameData();
    }
    waitFrameSlot();

    vkResetCommandBuffer(commandBuffers[currentFrame], 0);
    vkResetCommandBuffer(postViewportGraphicsCommandBuffers[currentFrame], 0);
    presentationGraphSubmittedThisFrame = false;
    {
        GE_PROFILE_SCOPE("GPU Object Update");
        updateRenderableBuffers();
    }
    const Vec3 sceneCameraPosition = cameraController.editorPosition();
    const float sceneCameraYaw = cameraController.editorYaw();
    const float sceneCameraPitch = cameraController.editorPitch();
    const bool sceneCameraChanged =
        !sceneViewportCacheValid || sceneCameraPosition.x() != renderedSceneViewportPosition.x() ||
        sceneCameraPosition.y() != renderedSceneViewportPosition.y() ||
        sceneCameraPosition.z() != renderedSceneViewportPosition.z() || sceneCameraYaw != renderedSceneViewportYaw ||
        sceneCameraPitch != renderedSceneViewportPitch;
    // Both paths retain the Scene View result until the camera, scene,
    // viewport, or explicit editor state requests a redraw. The direct
    // path uses a pass whose input and output layouts are sampled.
    // Water animation is time-dependent, therefore a cached Scene
    // View cannot remain valid while virtual water is active.
    sceneViewportRendered =
        sceneViewportActive &&
        (sceneViewportNeedsRender || sceneCameraChanged || scene.mutationRevision() != sceneViewportRenderedRevision ||
         sceneDescriptorPass.hasPendingPageUpdates() ||
         std::ranges::any_of(sceneVsmFeedbackPending, [](bool pending) { return pending; }) ||
         (sceneFrameDataCache.hasWind && !sceneGpu.grassInstances.empty()) ||
         renderSettingsRevision != sceneViewportRenderedSettingsRevision || sceneVirtualWaterRenderer.active());
    const bool renderGameViewport = !editorUiActive || !sceneViewportActive;
    if (renderGameViewport != gameShadowContextActive) {
        shadowPass.invalidateCache();
        sceneDescriptorPass.invalidateCache();
        shadowClipmapsValid = false;
        sceneShadowClipmapsValid = false;
        gameShadowContextActive = renderGameViewport;
        vsmRequestsReady.fill(false);
        sceneVsmFeedbackPending.fill(false);
        sceneVsmFeedbackCache = {};
        sceneVsmReceiverFeedbackValid = false;
    }
    // A slot without a Game View depth pass cannot provide history
    // to the next frame, even if it contains an older pyramid.
    if (!renderGameViewport) {
        hiZValid[currentFrame] = false;
    }
    // Scene View is deliberately not rendered in play mode. Its cache
    // cannot consume this frame's dirty list, so discard it lazily;
    // the active game-view cache is updated page-by-page below.
    if (!sceneViewportActive && !dirtyShadowObjects.empty()) {
        sceneDescriptorPass.invalidateCache();
    }
    {
        GE_PROFILE_SCOPE("Update Uniforms");
        updateUniformBuffer(currentFrame);
    }
    if (sceneViewportRendered) {
        updateSceneViewportUniformBuffer(currentFrame);
    }
    updateParticleSystemForFrame();
    updateCullingUniformBuffer(currentFrame);
    updateMeshletCullingUniformBuffer(currentFrame);
    if (sceneViewportRendered) {
        updateSceneCullingUniformBuffer(currentFrame);
    }
    const DirectionalLight& mainLight = sceneFrameDataCache.data.directionalLight;
    if (mainLight.enabled && mainLight.castShadows && optimizationFeatures.shadows && hasShadowCasters) {
        updateShadowCullingUniformBuffer(currentFrame);
    }
    // Apply a Resolution scale edit before descriptor updates and
    // command recording; this is a no-op unless its target extent changed.
    createDirectionalVisibilityPass();
    createRtContactShadowPass();
    createLocalLightShadowPass();
    {
        GE_PROFILE_SCOPE("Command Recording");
        recordCommandBuffer(commandBuffers[currentFrame]);
    }
    if (!swapchainImageAcquired) {
        // The swapchain went out of date after this frame's earlier work was
        // submitted; recreation idles the device, which retires that work.
        recreateSwapChain();
        return;
    }
    {
        GE_PROFILE_SCOPE("Queue Submit / Present");
        submitAndPresentFrame(acquiredImageIndex);
    }
    // Only this path records timestamp queries.  drawCoreFrame() shares
    // submitAndPresentFrame(), but does not reset or write this pool.
    gpuTimestampProfiler.markSubmitted(currentFrame, Profiler::currentFrameNumber());
    if (sceneViewportRendered) {
        renderedSceneViewportPosition = sceneCameraPosition;
        renderedSceneViewportYaw = sceneCameraYaw;
        renderedSceneViewportPitch = sceneCameraPitch;
        sceneViewportRenderedRevision = scene.mutationRevision();
        sceneViewportRenderedSettingsRevision = renderSettingsRevision;
        sceneViewportNeedsRender = false;
        sceneViewportCacheValid = true;
        sceneViewportImageInitialized = true;
    }
    if (sceneViewportImageInitialized && sceneViewportDescriptor == VK_NULL_HANDLE) {
        if (editorUiBackend)
            sceneViewportDescriptor = reinterpret_cast<VkDescriptorSet>(
                editorUiBackend->addTexture(reinterpret_cast<std::uint64_t>(sceneViewportTarget.color().imageView()),
                                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL));
    }

    currentFrame = (currentFrame + 1) % MAX_FRAMES_IN_FLIGHT;
    ++shadowClipFrameIndex;
}

void drawCoreFrame() {
    if (!hasDrawableExtent())
        return;
    uint32_t imageIndex;
    if (!acquireFrameImage(imageIndex))
        return;
    presentationGraphSubmittedThisFrame = false;
    VkCommandBuffer commandBuffer = commandBuffers[currentFrame];
    vkResetCommandBuffer(commandBuffer, 0);
    VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    if (vkBeginCommandBuffer(commandBuffer, &beginInfo) != VK_SUCCESS) {
        throw std::runtime_error("Could not begin core command buffer");
    }
    VkImageMemoryBarrier2 toColor{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    // Chains with the acquire semaphore's COLOR_ATTACHMENT_OUTPUT wait.
    toColor.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    toColor.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    toColor.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    toColor.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    toColor.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    toColor.image = swapchain.images().at(imageIndex);
    toColor.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo toColorDependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    toColorDependency.imageMemoryBarrierCount = 1;
    toColorDependency.pImageMemoryBarriers = &toColor;
    vkCmdPipelineBarrier2(commandBuffer, &toColorDependency);
    VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    color.imageView = swapchain.imageViews().at(imageIndex);
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.clearValue.color = {{0.06F, 0.07F, 0.09F, 1.0F}};
    VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
    rendering.renderArea.extent = swapchain.extent();
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &color;
    vkCmdBeginRendering(commandBuffer, &rendering);
    if (editorUiBackend)
        editorUiBackend->renderDrawData(reinterpret_cast<std::uint64_t>(commandBuffer));
    vkCmdEndRendering(commandBuffer);
    VkImageMemoryBarrier2 toPresent{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    toPresent.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    toPresent.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    toPresent.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    toPresent.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    toPresent.image = swapchain.images().at(imageIndex);
    toPresent.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo toPresentDependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    toPresentDependency.imageMemoryBarrierCount = 1;
    toPresentDependency.pImageMemoryBarriers = &toPresent;
    vkCmdPipelineBarrier2(commandBuffer, &toPresentDependency);
    if (vkEndCommandBuffer(commandBuffer) != VK_SUCCESS) {
        throw std::runtime_error("Could not end core command buffer");
    }
    submitAndPresentFrame(imageIndex);
    currentFrame = (currentFrame + 1) % MAX_FRAMES_IN_FLIGHT;
}

void updateFpsCounter() {
    fpsFrameCount++;
    fpsElapsedTime += Time::unscaledDeltaTime();

    if (fpsElapsedTime >= 1.0) {
        const double fps = fpsFrameCount / fpsElapsedTime;

        constexpr std::size_t windowTitleBufferSize{128};
        char title[windowTitleBufferSize];
        snprintf(title, sizeof(title), "GamEngine | FPS: %.1F | Renderables: %zu", fps, renderables.size());
        SDL_SetWindowTitle(window, title);

        fpsFrameCount = 0;
        fpsElapsedTime = 0.0;
    }
}

// ---------- CLEANUP ----------
