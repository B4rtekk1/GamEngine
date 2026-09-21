        static std::array<glm::vec4, 6> extractFrustumPlanes(const glm::mat4& matrix) {
            const glm::vec4 row0{matrix[0][0], matrix[1][0], matrix[2][0], matrix[3][0]};
            const glm::vec4 row1{matrix[0][1], matrix[1][1], matrix[2][1], matrix[3][1]};
            const glm::vec4 row2{matrix[0][2], matrix[1][2], matrix[2][2], matrix[3][2]};
            const glm::vec4 row3{matrix[0][3], matrix[1][3], matrix[2][3], matrix[3][3]};
            std::array<glm::vec4, 6> planes{
                row3 + row0, row3 - row0,
                row3 + row1, row3 - row1,
                row2,        row3 - row2};

            for (glm::vec4& plane : planes) {
                const float length = glm::length(glm::vec3{plane});
                if (length > 1.0e-6F) plane /= length;
            }
            return planes;
        }

        [[nodiscard]] DirectionalLight directionalLight() const noexcept {
            return sceneFrameDataCache.data.directionalLight;
        }

        void refreshSceneFrameData() {
            const Registry& readRegistry = registry;
            const std::uint64_t transformRevision = readRegistry.componentRevision<Transform>();
            const std::uint64_t lightRevision = readRegistry.componentRevision<LightComponent>();
            const std::uint64_t windRevision = readRegistry.componentRevision<WindComponent>();
            const std::uint64_t cameraRevision = readRegistry.componentRevision<CameraComponent>();
            const std::uint64_t uuidRevision = readRegistry.componentRevision<UUIDComponent>();
            const std::uint64_t structuralRevision = readRegistry.structuralRevision();
            const bool uuidIndexDirty = !sceneFrameDataCache.initialized ||
                sceneFrameDataCache.uuidRevision != uuidRevision ||
                sceneFrameDataCache.structuralRevision != structuralRevision;
            if (uuidIndexDirty) {
                auto& entitiesByUuid = sceneFrameDataCache.entitiesByUuid;
                entitiesByUuid.clear();
                entitiesByUuid.reserve(readRegistry.size());
                readRegistry.view<UUIDComponent>([&](const Entity entity, const UUIDComponent& uuid) {
                    entitiesByUuid.emplace(uuid.value, entity);
                });
            }

            // TransformSystem already records the world transforms it actually
            // recomputed, including descendants of a moved parent. A moving
            // renderable must not force scans of every camera, light and wind
            // component just because Transform has a global revision.
            bool relevantTransformChanged = !sceneFrameDataCache.initialized;
            if (!relevantTransformChanged &&
                sceneFrameDataCache.transformRevision != transformRevision) {
                for (const Entity entity : TransformSystem::changedWorldTransforms(registry)) {
                    if (readRegistry.has<CameraComponent>(entity) ||
                        readRegistry.has<LightComponent>(entity) ||
                        readRegistry.has<WindComponent>(entity)) {
                        relevantTransformChanged = true;
                        break;
                    }
                }
            }

            const bool dataDirty = !sceneFrameDataCache.initialized ||
                relevantTransformChanged ||
                sceneFrameDataCache.lightRevision != lightRevision ||
                sceneFrameDataCache.windRevision != windRevision ||
                sceneFrameDataCache.cameraRevision != cameraRevision ||
                sceneFrameDataCache.uuidRevision != uuidRevision;
            if (dataDirty) {
                SceneFrameData data{};
                bool directionalLightFound = false;
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
                        if (!light.enabled) return;
                        const glm::mat4 world = transform.worldMatrix().native();
                        const glm::vec3 direction = glm::vec3(
                            world * glm::vec4{0.0F, 0.0F, -1.0F, 0.0F});
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
                        if (data.lightCount == MaxLocalLights || light.range <= 0.0F) return;
                        constexpr float pi = 3.14159265358979323846F;
                        auto& gpu = data.lights[data.lightCount++];
                        const glm::vec3 position = glm::vec3{world[3]};
                        gpu.positionRange = {position.x, position.y, position.z, light.range};
                        gpu.directionOuterCos = {glm::normalize(direction),
                                                 std::cos(light.outerConeAngle * pi / 180.0F)};
                        gpu.colorIntensity = {color.r(), color.g(), color.b(), std::max(0.0F, light.intensity)};
                        gpu.parameters = {std::cos(light.innerConeAngle * pi / 180.0F),
                                          static_cast<float>(light.type), light.castShadows ? 1.0F : 0.0F, 0.0F};
                    });
                sceneFrameDataCache.hasWind = false;
                readRegistry.view<Transform, WindComponent>(
                    [&](const Entity, const Transform& transform, const WindComponent& wind) {
                        if (sceneFrameDataCache.hasWind || !wind.enabled || wind.strength <= 0.0F ||
                            wind.range <= 0.0F) return;
                        const float length = wind.direction.length();
                        if (length <= 1.0e-4F) return;
                        const Vec3 direction = wind.direction * (1.0F / length);
                        data.wind.directionStrength = {direction.x(), direction.y(), direction.z(), wind.strength};
                        const glm::vec3 position = glm::vec3{transform.worldMatrix().native()[3]};
                        data.wind.sourcePositionRange = {position.x, position.y, position.z, wind.range};
                        data.wind.gustFrequencyTime = {wind.gustStrength, wind.frequency, 0.0F, 0.0F};
                        sceneFrameDataCache.hasWind = true;
                    });
                if (data.primaryCamera != NullEntity) {
                    const Transform& cameraTransform = readRegistry.get<Transform>(data.primaryCamera);
                    sceneFrameDataCache.primaryCameraPosition = Vec3{glm::vec3{cameraTransform.worldMatrix().native()[3]}};
                    sceneFrameDataCache.primaryCameraYaw = cameraTransform.rotation.y();
                    sceneFrameDataCache.primaryCameraPitch = cameraTransform.rotation.x();
                    Entity current = data.primaryCamera;
                    while (readRegistry.has<ParentComponent>(current)) {
                        const UUID parentUuid = readRegistry.get<ParentComponent>(current).parentUuid;
                        const auto parent = sceneFrameDataCache.entitiesByUuid.find(parentUuid);
                        if (parent == sceneFrameDataCache.entitiesByUuid.end() ||
                            !readRegistry.has<Transform>(parent->second)) break;
                        current = parent->second;
                        const Transform& parentTransform = readRegistry.get<Transform>(current);
                        sceneFrameDataCache.primaryCameraYaw += parentTransform.rotation.y();
                        sceneFrameDataCache.primaryCameraPitch += parentTransform.rotation.x();
                    }
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
            sceneFrameDataCache.data.wind.gustFrequencyTime = {
                cachedWind.x(), cachedWind.y(), now, now - static_cast<float>(Time::deltaTime())};
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
            bool probesChanged = !reflectionProbeTableInitialized ||
                componentRevision != reflectionProbeComponentRevision;
            if (!probesChanged && transformRevision != reflectionProbeTransformRevision) {
                registry.forEachComponentChangedSince<Transform>(reflectionProbeTransformRevision,
                    [&](const Entity entity) {
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
                    sizeof(GpuReflectionProbe) * std::max<std::size_t>(1, reflectionProbes.size()));
                reflectionProbeManager.uploadProbeTable(frame);
                reflectionProbeBufferDirtyMask &= ~frameBit;
            }
            const auto& reflectionProbes = reflectionProbeManager.probes();
            const bool renderGameViewport = !editorUiActive || !sceneViewportActive;
            const SceneFrameData& frameData = sceneFrameDataCache.data;
            const bool mainLightShadows = frameData.directionalLight.enabled &&
                frameData.directionalLight.castShadows && optimizationFeatures.shadows && hasShadowCasters;
            const Entity activeCamera = frameData.primaryCamera;
            if (activeCamera == NullEntity) {
                // A malformed or incomplete scene must not stop rendering. Use
                // the editor camera as a predictable, controllable fallback
                // until the author adds or repairs a primary perspective camera.
                if (!fallbackCameraWarningReported) {
                    Diagnostics::instance().report(
                        DiagnosticSeverity::Warning,
                        "No usable primary camera; rendering with the fallback camera.",
                        {.subsystem = "Renderer", .component = "CameraComponent",
                         .suggestedAction = "Add a perspective camera with Transform and mark it Primary."});
                    fallbackCameraWarningReported = true;
                }
                cameraController.camera().emplace(
                    Degrees{SCENE_CAMERA_FOV_DEGREES}, 16.0F / 9.0F,
                    SCENE_CAMERA_NEAR_CLIP, SCENE_CAMERA_FAR_CLIP);
                cameraController.camera()->setPosition(cameraController.editorPosition());
                cameraController.camera()->setRotation(Degrees{cameraController.editorYaw()},
                                                        Degrees{cameraController.editorPitch()});
            } else {
                fallbackCameraWarningReported = false;
                const auto& component = sceneFrameDataCache.primaryCameraComponent;

                // Game View is presented in a fixed 16:9 editor frame. Keep the
                // projection in that aspect too, independently of dock layout.
                const float gameAspect = editorUiActive ? (16.0F / 9.0F) : component.aspectRatio;
                cameraController.camera().emplace(Degrees{component.fieldOfView}, gameAspect,
                                                   component.nearClip, component.farClip);
                // A camera's pitch is a view-space angle, not merely an X-axis
                // model rotation. Deriving it from the model matrix made pitch
                // depend on yaw, producing an unnatural vertical mouse look.
                cameraController.camera()->setPosition(sceneFrameDataCache.primaryCameraPosition);
                cameraController.camera()->setRotation(Degrees{sceneFrameDataCache.primaryCameraYaw},
                                                        Degrees{sceneFrameDataCache.primaryCameraPitch});
            }

            taaJitterX = 0.0F;
            taaJitterY = 0.0F;
            // The embedded Game View is also rendered before the ImGui pass.
            // Apply TAA there, but leave the independent Scene View camera
            // stable so editor gizmos and picking stay pixel-precise.
            const bool temporalAaWasActive = taaResolveActive;
            taaResolveActive = antialiasingLevel == AntialiasingLevel::TAA &&
                (!editorUiActive || !sceneViewportActive);
            if (taaResolveActive && !temporalAaWasActive) {
                temporalAaPass.reset();
                taaSampleIndex = 0;
            }
            if (taaResolveActive) {
                constexpr std::uint32_t TaaSampleCount = 8;
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
                const std::uint32_t sampleIndex =
                    static_cast<std::uint32_t>(taaSampleIndex % TaaSampleCount);
                taaJitterX = (halton(sampleIndex + 1, 2) - 0.5F) * 2.0F /
                             static_cast<float>(extent.width);
                taaJitterY = (halton(sampleIndex + 1, 3) - 0.5F) * 2.0F /
                             static_cast<float>(extent.height);
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
            const bool projectionChanged = previousGameCameraValid &&
                (std::abs(projection[0][0] - previousProjection[0][0]) > 1e-4F ||
                 std::abs(projection[1][1] - previousProjection[1][1]) > 1e-4F ||
                 std::abs(projection[2][2] - previousProjection[2][2]) > 1e-4F ||
                 std::abs(projection[3][2] - previousProjection[3][2]) > 1e-4F);
            const bool cameraCut = previousGameCameraValid &&
                ((currentCameraPosition - previousGameCameraPosition).length() > cameraCutDistance ||
                 dot(currentCameraForward, previousGameCameraForward) < cameraCutDirectionDot ||
                 activeCamera != previousGameCamera || projectionChanged);
            const std::uint32_t shadowPageBudget = (!previousGameCameraValid || cameraCut) ? 128u : 64u;

            std::array<std::uint32_t, ShadowMap::VirtualPageCount> completedVsmRequests{};
            std::span<const std::uint32_t> receiverPageRequests;
            // currentFrame's fence was waited before this per-frame update.
            // Reading this mapped allocation therefore consumes GPU work from
            // its prior use with no queue wait or submission-time readback.
            if (!cameraCut && vsmRequestsReady[currentFrame] &&
                vsmCompactedPageCountBuffers[currentFrame].handle() != VK_NULL_HANDLE) {
                std::uint32_t requestCount{};
                vsmCompactedPageCountBuffers[currentFrame].read(&requestCount, sizeof(requestCount));
                requestCount = std::min(requestCount, ShadowMap::VirtualPageCount);
                if (requestCount != 0) {
                    vsmCompactedPageBuffers[currentFrame].read(completedVsmRequests.data(),
                        sizeof(std::uint32_t) * requestCount);
                    receiverPageRequests = std::span{completedVsmRequests}.first(requestCount);
                }
            }

            if (mainLightShadows && renderGameViewport) {
                shadowClipUpdateMask = updateVirtualShadowClipmaps(
                    cameraController.camera()->position(), shadowClipMatrices,
                    lastShadowCameraPosition, lastShadowLightDirection, shadowClipmapsValid);
                shadowPass.preparePages(
                    shadowClipMatrices,
                    cameraController.camera()->projectionMatrix() *
                        cameraController.camera()->viewMatrix(),
                    gpuObjects, dirtyShadowObjects, receiverPageRequests, currentFrame, shadowPageBudget);
            } else {
                shadowClipUpdateMask = 0;
                shadowClipmapsValid = false;
                shadowPass.invalidateCache();
                vsmRequestsReady.fill(false);
            }
            if (vsmPageMarkingUniformBuffers[currentFrame].handle() != VK_NULL_HANDLE) {
                VsmPageMarkingUniforms marking{};
                // Page marking samples the Hi-Z image from this frame slot.
                // That image was produced when this slot was last rendered,
                // so reconstruct with the VP stored alongside that Hi-Z
                // image—not the camera from the immediately preceding frame.
                marking.inverseViewProjection = glm::inverse(hiZViewProjections[currentFrame]);
                for (std::uint32_t level = 0; level < ShadowMap::ClipLevelCount; ++level)
                    marking.clipMatrices[level] = shadowClipMatrices[level].native();
                marking.pageCountPerAxis = ShadowMap::VirtualPagesPerAxis;
                marking.virtualResolution = ShadowMap::VirtualResolution;
                marking.clipLevelCount = ShadowMap::ClipLevelCount;
                marking.depthWidth = swapchain.extent().width;
                marking.depthHeight = swapchain.extent().height;
                marking.shadowQuality = static_cast<std::uint32_t>(shadowQuality);
                vsmPageMarkingUniformBuffers[currentFrame].update(&marking, sizeof(marking));
            }
            // Motion vectors describe continuous motion.  Reusing history after
            // a teleport or a large orientation jump produces unavoidable
            // ghosting, so treat it as a camera cut instead.
            if (cameraCut) {
                // A discontinuous camera/projection change invalidates every
                // temporal Hi-Z slot, not only the slot rendered this frame.
                hiZValid.fill(false);
                virtualWaterRenderer.invalidateTemporalHistory();
                // GTAO history is independent of TAA and must not survive a
                // teleport or a large camera rotation either.
                gtaoPass.reset();
                if (taaResolveActive) temporalAaPass.reset();
            }
            const UniformBufferObject data{
                currentView, currentProjection, Mat4{glm::inverse(currentProjection.native())},
                Mat4{glm::inverse(currentView.native())},
                previousGameCameraValid ? previousGameView : currentView,
                previousGameCameraValid ? previousGameProjection : currentProjection,
                shadowClipMatrices,
                Vec4{cameraController.camera()->position().x(), cameraController.camera()->position().y(),
                     cameraController.camera()->position().z(), 1.0F},
                Vec4{frameData.directionalLight.direction.x(), frameData.directionalLight.direction.y(),
                     frameData.directionalLight.direction.z(), frameData.directionalLight.intensity},
                Vec4{frameData.directionalLight.color.r(), frameData.directionalLight.color.g(),
                     frameData.directionalLight.color.b(), 1.0F},
                frameData.wind.directionStrength, frameData.wind.sourcePositionRange,
                frameData.wind.gustFrequencyTime,
                mainLightShadows ? 1u : 0u,
                static_cast<std::uint32_t>(shadowQuality),
                static_cast<std::uint32_t>(shadowDebugView),
                static_cast<std::uint32_t>(gtaoDebugView),
                materialSlots, editorSelectedRenderable, frameData.lightCount,
                static_cast<std::uint32_t>(reflectionProbes.size()),
                1u,
                // Do not vary PCF/VSM sample phase until TAA has stronger
                // per-surface confidence (normals/reactive mask).  Depth
                // rejection prevents trails, but cannot fully hide changing
                // shadow-filter noise at sub-pixel edges.
                0u,
                static_cast<std::uint32_t>(Profiler::currentFrameNumber()),
                static_cast<std::uint32_t>(pbrDebugView),
                glm::vec4{static_cast<float>(swapchain.extent().width),
                          static_cast<float>(swapchain.extent().height), 0.1F, 1000.0F},
                frameData.lights};
            uniformBuffers[frame].update(&data, sizeof(data));
            const ClusteredLightingUniforms clustered{
                currentView.native(), currentProjection.native(),
                glm::uvec4{(swapchain.extent().width + ClusterTileSize - 1U) / ClusterTileSize,
                           (swapchain.extent().height + ClusterTileSize - 1U) / ClusterTileSize,
                           ClusterDepthSlices, frameData.lightCount},
                glm::vec4{static_cast<float>(swapchain.extent().width), static_cast<float>(swapchain.extent().height), 0.1F, 1000.0F}};
            clusteredLightingUniformBuffers[frame].update(&clustered, sizeof(clustered));
            particlePreviousGameViewProjection = previousGameCameraValid
                ? previousGameProjection * previousGameView : currentProjection * currentView;
            particlePreviousGameCameraRight = previousGameCameraValid
                ? previousGameCameraRight : cameraController.camera()->right();
            particlePreviousGameCameraUp = previousGameCameraValid
                ? previousGameCameraUp : cameraController.camera()->up();
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
            const bool mainLightShadows = frameData.directionalLight.enabled &&
                frameData.directionalLight.castShadows && optimizationFeatures.shadows && hasShadowCasters;
            const float aspect = static_cast<float>(sceneViewportTarget.extent().width) /
                                 static_cast<float>(sceneViewportTarget.extent().height);
            Camera sceneCamera{Degrees{60.0F}, aspect, 0.1F, 1000.0F};
            sceneCamera.setPosition(cameraController.editorPosition());
            sceneCamera.setRotation(Degrees{cameraController.editorYaw()},
                                    Degrees{cameraController.editorPitch()});
            if (mainLightShadows) {
                sceneShadowClipUpdateMask = updateVirtualShadowClipmaps(
                    sceneCamera.position(), sceneShadowClipMatrices,
                    lastSceneShadowCameraPosition, lastSceneShadowLightDirection,
                    sceneShadowClipmapsValid);
                sceneDescriptorPass.preparePages(
                    sceneShadowClipMatrices,
                    sceneCamera.projectionMatrix() * sceneCamera.viewMatrix(),
                    gpuObjects, dirtyShadowObjects, {}, currentFrame, 32);
            } else {
                sceneShadowClipUpdateMask = 0;
                sceneShadowClipmapsValid = false;
                sceneDescriptorPass.invalidateCache();
            }
            const Mat4 sceneView = sceneCamera.viewMatrix();
            const Mat4 sceneProjection = sceneCamera.projectionMatrix();
            const UniformBufferObject data{
                sceneView, sceneProjection, Mat4{glm::inverse(sceneProjection.native())},
                Mat4{glm::inverse(sceneView.native())}, sceneView, sceneProjection,
                sceneShadowClipMatrices,
                Vec4{sceneCamera.position().x(), sceneCamera.position().y(), sceneCamera.position().z(), 1.0F},
                Vec4{frameData.directionalLight.direction.x(), frameData.directionalLight.direction.y(),
                     frameData.directionalLight.direction.z(), frameData.directionalLight.intensity},
                Vec4{frameData.directionalLight.color.r(), frameData.directionalLight.color.g(),
                     frameData.directionalLight.color.b(), 1.0F},
                frameData.wind.directionStrength, frameData.wind.sourcePositionRange,
                frameData.wind.gustFrequencyTime,
                mainLightShadows ? 1u : 0u,
                static_cast<std::uint32_t>(shadowQuality),
                static_cast<std::uint32_t>(shadowDebugView),
                0u,
                materialSlots, editorSelectedRenderable, frameData.lightCount,
                static_cast<std::uint32_t>(reflectionProbeManager.probes().size()),
                0u,
                0u, 0u, static_cast<std::uint32_t>(pbrDebugView),
                glm::vec4{static_cast<float>(sceneViewportTarget.extent().width),
                          static_cast<float>(sceneViewportTarget.extent().height), 0.1F, 1000.0F},
                frameData.lights};
            sceneUniformBuffers[frame].update(&data, sizeof(data));
            const ClusteredLightingUniforms clustered{
                sceneView.native(), sceneProjection.native(),
                glm::uvec4{(sceneViewportTarget.extent().width + ClusterTileSize - 1U) / ClusterTileSize,
                           (sceneViewportTarget.extent().height + ClusterTileSize - 1U) / ClusterTileSize,
                           ClusterDepthSlices, frameData.lightCount},
                glm::vec4{static_cast<float>(sceneViewportTarget.extent().width), static_cast<float>(sceneViewportTarget.extent().height), 0.1F, 1000.0F}};
            sceneClusteredLightingUniformBuffers[frame].update(&clustered, sizeof(clustered));
        }

        struct SwapchainImageIndex final {
            uint32_t value;
        };

        void recordCommandBuffer(VkCommandBuffer commandBuffer, const SwapchainImageIndex imageIndex) {
            VkCommandBufferBeginInfo beginInfo{};
            beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;

            if (vkBeginCommandBuffer(commandBuffer, &beginInfo) != VK_SUCCESS) {
                throw std::runtime_error("Could not begin command buffer");
            }
            gpuTimestampProfiler.beginFrame(commandBuffer, currentFrame);
            static const ProfileNameId shadowProfileName = Profiler::registerName("Shadow");
            static const ProfileNameId shadowPageMarkProfileName = Profiler::registerName("Shadow.PageMark");
            static const ProfileNameId shadowPageCompactProfileName = Profiler::registerName("Shadow.PageCompact");
            static const ProfileNameId shadowDepthRasterProfileName = Profiler::registerName("Shadow.CasterCullDepth");
            static const ProfileNameId shadowProjectionProfileName = Profiler::registerName("Shadow.Projection");
            static const ProfileNameId cullingProfileName = Profiler::registerName("Culling");
            static const ProfileNameId forwardProfileName = Profiler::registerName("Forward");
            static const ProfileNameId velocityProfileName = Profiler::registerName("Velocity");
            static const ProfileNameId taaProfileName = Profiler::registerName("TAA");
            static const ProfileNameId bloomProfileName = Profiler::registerName("Bloom");
            static const ProfileNameId tonemapProfileName = Profiler::registerName("Tonemap");
            static const ProfileNameId rtTlasProfileName = Profiler::registerName("RT TLAS");
            static const ProfileNameId rtContactProfileName = Profiler::registerName("RT Contact Shadows");
            const bool renderSceneViewport = editorUiActive && sceneViewportRendered;
            // Scene View replaces the embedded Game View in the editor.  Do
            // not submit hidden Game View work: this also makes the shared
            // physical VSM atlas single-writer for the entire frame.
            const bool renderGameViewport = !editorUiActive || !sceneViewportActive;
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
            std::vector<RenderGraph::BufferHandle> frameUploadBuffers;
            const auto importFrameUploadBuffer = [&](const char* name, const Buffer& buffer) {
                if (buffer.handle() == VK_NULL_HANDLE || buffer.size() == 0) return;
                const auto handle = frameGraph.importBuffer(name, buffer.handle(), {
                    .size = buffer.size(), .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT});
                if (buffer.readyTimeline() != 0) frameGraph.markUploaded(handle, buffer.readyTimeline());
                frameUploadBuffers.push_back(handle);
            };
            importFrameUploadBuffer("Geometry vertices", vertexBuffer);
            importFrameUploadBuffer("Geometry indices", indexBuffer);
            importFrameUploadBuffer("Frame instances", instanceBuffers[currentFrame]);
            importFrameUploadBuffer("Frame materials", materialBuffers[currentFrame]);
            importFrameUploadBuffer("GPU scene instances", gpuSceneInstanceBuffers[currentFrame]);
            importFrameUploadBuffer("GPU scene meshes", gpuSceneMeshBuffers[currentFrame]);
            importFrameUploadBuffer("GPU scene materials", gpuSceneMaterialBuffers[currentFrame]);
            importFrameUploadBuffer("Frame lights", uniformBuffers[currentFrame]);
            importFrameUploadBuffer("Shadow culling", shadowCullingUniformBuffers[currentFrame]);
            importFrameUploadBuffer("Clustered lighting", clusteredLightingUniformBuffers[currentFrame]);
            importFrameUploadBuffer("Scene clustered lighting", sceneClusteredLightingUniformBuffers[currentFrame]);
            if (particleSystem && particleSystem->particleBuffer() != VK_NULL_HANDLE) {
                const auto particleBuffer = frameGraph.importBuffer("Particle state", particleSystem->particleBuffer(), {
                    .size = particleSystem->particleBufferSize(), .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT});
                if (particleSystem->readyTimeline() != 0)
                    frameGraph.markUploaded(particleBuffer, particleSystem->readyTimeline());
                frameUploadBuffers.push_back(particleBuffer);
            }
            std::vector<RenderGraph::TextureHandle> frameUploadTextures;
            const auto importFrameUploadTexture = [&](const char* name, const Texture2D& texture) {
                if (!texture.valid()) return;
                const auto handle = frameGraph.importTexture(name, texture.image(), {
                    .extent = {texture.width(), texture.height(), 1}, .format = texture.format(),
                    .usage = VK_IMAGE_USAGE_SAMPLED_BIT, .aspect = VK_IMAGE_ASPECT_COLOR_BIT,
                    .mipLevels = texture.mipLevels()}, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                if (texture.readyTimeline() != 0) frameGraph.markUploaded(handle, texture.readyTimeline());
                frameUploadTextures.push_back(handle);
            };
            importFrameUploadTexture("Fallback material", fallbackMaterialTexture);
            importFrameUploadTexture("Grass height", grassHeightTexture);
            importFrameUploadTexture("Grass density", grassDensityTexture);
            for (const Texture2D& texture : materialTextures) importFrameUploadTexture("Material texture", texture);
            frameGraph.addPass("Legacy frame resource consumers", RenderGraph::Queue::Graphics,
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
                }, [](VkCommandBuffer) {});
            // TAA consumes the Virtual Water prepass attachments.  Keep this
            // frame-local contract separate from whether the water world has
            // bodies, as a renderer can be active without its prepass having
            // been recorded yet.
            // The virtual-water prepass is recorded by the legacy work pass
            // below whenever the Game View and the renderer are active. Keep
            // this decision available while declaring TAA's graph resources.
            const bool virtualWaterPreparedThisFrame = renderGameViewport && virtualWaterRenderer.active();
            // Until each callback receives a precise resource declaration,
            // keep the legacy recording sequence as one ordered frame-graph
            // node. This is deliberately a side-effect root: it records
            // shadowing, culling, raster, water and Scene View work which is
            // still described by their legacy descriptor sets.
            frameGraph.addPass("Legacy frame work", RenderGraph::Queue::Graphics,
                [](RenderGraph::PassBuilder& builder) { builder.setSideEffect(); },
                [&](const VkCommandBuffer legacyCommandBuffer) {
            // All code in this block records into the same command buffer.
            // Bind the graph-owned command buffer by reference so accidental
            // future command-buffer splits cannot bypass the graph schedule.
            commandBuffer = legacyCommandBuffer;
            if (meshShaderPathActive && vulkanDevice.supportsMeshShaders() && globalMeshletCount != 0 &&
                !sceneGpu.database.instances().empty() &&
                meshletCullSets[currentFrame] != VK_NULL_HANDLE) {
                // Build an indirect command from the compact coarse-visible
                // list, so meshlet culling launches one group per visible
                // instance instead of one per instance in the whole scene.
                instanceCullingPasses[currentFrame].record(commandBuffer,
                    static_cast<std::uint32_t>(sceneGpu.database.instances().size()));
                vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, meshletDispatchPipeline);
                const VkDescriptorSet meshletDispatchSet = meshletDispatchSets[currentFrame];
                vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                                        meshletDispatchPipelineLayout, 0, 1, &meshletDispatchSet, 0, nullptr);
                vkCmdDispatch(commandBuffer, 1, 1, 1);
                const VkBufferMemoryBarrier2 meshletDispatchBarrier{
                    .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
                    .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                    .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                    .dstStageMask = VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT,
                    .dstAccessMask = VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT,
                    .buffer = meshletCullDispatchBuffers[currentFrame].handle(),
                    .offset = 0, .size = sizeof(VkDispatchIndirectCommand)};
                const VkDependencyInfo meshletDispatchDependency{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                    .bufferMemoryBarrierCount = 1, .pBufferMemoryBarriers = &meshletDispatchBarrier};
                vkCmdPipelineBarrier2(commandBuffer, &meshletDispatchDependency);
                vkCmdFillBuffer(commandBuffer, visibleMeshletCountBuffers[currentFrame].handle(), 0,
                                sizeof(std::uint32_t), 0);
                const VkMemoryBarrier2 clearBarrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, nullptr,
                    VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT};
                const VkDependencyInfo clearDependency{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                    .memoryBarrierCount = 1, .pMemoryBarriers = &clearBarrier};
                vkCmdPipelineBarrier2(commandBuffer, &clearDependency);
                vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, meshletCullingPipeline);
                const VkDescriptorSet meshletSet = meshletCullSets[currentFrame];
                vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                                        meshletCullingPipelineLayout, 0, 1, &meshletSet, 0, nullptr);
                vkCmdDispatchIndirect(commandBuffer, meshletCullDispatchBuffers[currentFrame].handle(), 0);
                const VkMemoryBarrier2 meshletBarrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, nullptr,
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                    VK_ACCESS_2_SHADER_STORAGE_READ_BIT};
                const VkDependencyInfo meshletDependency{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                    .memoryBarrierCount = 1, .pMemoryBarriers = &meshletBarrier};
                vkCmdPipelineBarrier2(commandBuffer, &meshletDependency);
                vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, meshletIndirectPipeline);
                const VkDescriptorSet meshletIndirectSet = meshletIndirectSets[currentFrame];
                vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                                        meshletIndirectPipelineLayout, 0, 1, &meshletIndirectSet, 0, nullptr);
                vkCmdDispatch(commandBuffer, 1, 1, 1);
                const VkMemoryBarrier2 meshTaskIndirectBarrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, nullptr,
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_MESH_SHADER_BIT_EXT,
                    VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT};
                const VkDependencyInfo meshTaskIndirectDependency{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                    .memoryBarrierCount = 1, .pMemoryBarriers = &meshTaskIndirectBarrier};
                vkCmdPipelineBarrier2(commandBuffer, &meshTaskIndirectDependency);
            }
            const ForwardPass& sceneForwardPass = msaa.enabled()
                ? forwardPass
                : sceneViewportForwardPass;
            const DirectionalLight& mainLight = sceneFrameDataCache.data.directionalLight;
            const bool mainLightShadows = mainLight.enabled && mainLight.castShadows &&
                optimizationFeatures.shadows && hasShadowCasters;
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
            // Culling runs before this frame's depth pass, so it consumes the
            // Hi-Z result from the previous frame. On the first frame there is
            // no previous result, but the descriptor is still bound and the
            // image must be in the layout declared in that descriptor. The
            // culling uniform's cameraCut flag disables occlusion testing for
            // this frame, so an undefined image contents is acceptable after
            // this layout transition.
            // Empty scenes do not allocate culling/Hi-Z resources. Keep the
            // frame path disabled for them so no barrier references the null
            // image handle left by the intentionally skipped allocation.
            const auto& hiZBuffer = hiZBuffers[currentFrame];
            const bool hasHiZResources = hiZBuffer.image() != VK_NULL_HANDLE;
            const bool hizEnabled = canUseHiZOcclusionCulling() && hasHiZResources;
            const bool hadPreviousHiZ = hiZValid[currentFrame];
            // The culling descriptor set always contains the Hi-Z image. Keep
            // its layout valid before the compute culling dispatch, even when
            // that dispatch skips occlusion testing.
            if (hasHiZResources && !hadPreviousHiZ) {
                VkImageMemoryBarrier2 initialBarrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
                initialBarrier.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
                initialBarrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                initialBarrier.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
                initialBarrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                initialBarrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                initialBarrier.image = hiZBuffer.image();
                initialBarrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0,
                                                   hiZBuffer.mipCount(), 0, 1};

                VkDependencyInfo initialDependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
                initialDependency.imageMemoryBarrierCount = 1;
                initialDependency.pImageMemoryBarriers = &initialBarrier;
                vkCmdPipelineBarrier2(commandBuffer, &initialDependency);
            }

            if (particleSystem) {
                particleSystem->recordCompute(commandBuffer, particleComputePipeline,
                                              particleComputePipelineLayout, currentFrame);
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
                const GrassClassifyUniformData classifyData{
                    grassSettings.renderDistance, grassSettings.shadowDistance,
                    grassSettings.velocityDistance, 0.0F,
                    glm::vec4{camera->position().native(), 1.0F}};
                grassClassifyUniformBuffers[currentFrame].update(&classifyData, sizeof(classifyData));
                const GrassPackedStreamUniformData streamData{static_cast<uint32_t>(sceneGpu.grassInstances.size()), 0U, static_cast<uint32_t>(sceneGpu.grassClusters.size()), 0U};
                for (uint32_t stream = 0; stream < 3; ++stream) { auto data = streamData; data.streamIndex = stream; grassPackedStreamUniformBuffers[currentFrame][stream].update(&data, sizeof(data)); }
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
                const VkMemoryBarrier2 transferToCompute{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, nullptr, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT};
                const VkDependencyInfo dependency{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &transferToCompute}; vkCmdPipelineBarrier2(commandBuffer, &dependency);
                const auto dispatch = [&](VkPipeline pipeline, VkPipelineLayout layout, VkDescriptorSet set, uint32_t groups) { vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline); vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, nullptr); vkCmdDispatch(commandBuffer, groups, 1, 1); const VkMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, nullptr, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT}; const VkDependencyInfo info{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &barrier}; vkCmdPipelineBarrier2(commandBuffer, &info); };
                dispatch(grassPackedCullPipeline, grassPackedCullPipelineLayout, grassPackedCullSets[currentFrame], (static_cast<uint32_t>(sceneGpu.grassClusters.size()) + 63U) / 64U);
                const VkMemoryBarrier2 clusterDispatchBarrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, nullptr, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT, VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT}; const VkDependencyInfo clusterDispatchInfo{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &clusterDispatchBarrier}; vkCmdPipelineBarrier2(commandBuffer, &clusterDispatchInfo);
                vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, grassBladeCullPipeline);
                const VkDescriptorSet bladeSet = grassBladeCullSets[currentFrame];
                vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, grassBladeCullPipelineLayout, 0, 1, &bladeSet, 0, nullptr);
                vkCmdDispatchIndirect(commandBuffer, lists.bladeCullDispatch.handle(), 0);
                const VkMemoryBarrier2 bladeBarrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, nullptr, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT}; const VkDependencyInfo bladeInfo{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &bladeBarrier}; vkCmdPipelineBarrier2(commandBuffer, &bladeInfo);
                dispatch(grassDispatchBuildPipeline, grassDispatchBuildPipelineLayout, grassVisibleDispatchBuildSets[currentFrame], 1);
                const VkMemoryBarrier2 dispatchBuildBarrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, nullptr, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT, VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT}; const VkDependencyInfo dispatchBuildInfo{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &dispatchBuildBarrier}; vkCmdPipelineBarrier2(commandBuffer, &dispatchBuildInfo);
                vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, grassClassifyPipeline); const VkDescriptorSet classifySet = grassClassifySets[currentFrame]; vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, grassClassifyPipelineLayout, 0, 1, &classifySet, 0, nullptr); vkCmdDispatchIndirect(commandBuffer, lists.dispatchIndirect.handle(), 0); vkCmdPipelineBarrier2(commandBuffer, &bladeInfo);
                dispatch(grassDispatchBuildPipeline, grassDispatchBuildPipelineLayout, grassStreamDispatchBuildSets[currentFrame], 1); vkCmdPipelineBarrier2(commandBuffer, &dispatchBuildInfo);
                for (uint32_t stream = 0; stream < 3; ++stream) { const VkDeviceSize dispatchOffset = sizeof(GrassBladeDispatchData) * stream; vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, grassPackedBinPipeline); const VkDescriptorSet binSet = grassPackedBinSets[currentFrame][stream]; vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, grassPackedBinPipelineLayout, 0, 1, &binSet, 0, nullptr); vkCmdDispatchIndirect(commandBuffer, lists.dispatchIndirect.handle(), dispatchOffset); vkCmdPipelineBarrier2(commandBuffer, &bladeInfo); dispatch(grassPackedPrefixPipeline, grassPrefixPipelineLayout, grassPackedPrefixSets[currentFrame][stream], 1); vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, grassPackedScatterPipeline); const VkDescriptorSet scatterSet = grassPackedScatterSets[currentFrame][stream]; vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, grassPackedScatterPipelineLayout, 0, 1, &scatterSet, 0, nullptr); vkCmdDispatchIndirect(commandBuffer, lists.dispatchIndirect.handle(), dispatchOffset); vkCmdPipelineBarrier2(commandBuffer, &bladeInfo); dispatch(grassPackedFinalizePipeline, grassPackedFinalizePipelineLayout, grassPackedFinalizeSets[currentFrame][stream], (static_cast<uint32_t>(sceneGpu.grassClusters.size()) + 63U) / 64U); }
                const VkMemoryBarrier2 grassDrawBarrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, nullptr,
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
                    VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT};
                const VkDependencyInfo grassDrawDependency{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                    .memoryBarrierCount = 1, .pMemoryBarriers = &grassDrawBarrier};
                vkCmdPipelineBarrier2(commandBuffer, &grassDrawDependency);
            }

            // The editor camera has an independent frustum.  Produce its own
            // packed-grass command list instead of reusing Game View results.
            if (renderSceneViewport && !sceneGpu.grassInstances.empty()) {
                auto& lists = sceneGrassRenderLists[currentFrame];
                const float aspect = static_cast<float>(sceneViewportTarget.extent().width) / static_cast<float>(sceneViewportTarget.extent().height);
                Camera sceneCamera{Degrees{60.0F}, aspect, 0.1F, 1000.0F};
                sceneCamera.setPosition(cameraController.editorPosition());
                sceneCamera.setRotation(Degrees{cameraController.editorYaw()}, Degrees{cameraController.editorPitch()});
                GrassPackedCullUniformData cullData{};
                cullData.viewProjection = sceneCamera.unjitteredProjectionMatrix().native() * sceneCamera.viewMatrix().native();
                cullData.cameraPosition = glm::vec4{sceneCamera.position().native(), 1.0F};
                cullData.clusterCount = static_cast<uint32_t>(sceneGpu.grassClusters.size());
                cullData.frustumPlanes = extractFrustumPlanes(cullData.viewProjection);
                sceneGrassPackedCullUniformBuffers[currentFrame].update(&cullData, sizeof(cullData));
                const GrassClassifyUniformData classifyData{
                    grassSettings.renderDistance, grassSettings.shadowDistance,
                    grassSettings.velocityDistance, 0.0F,
                    glm::vec4{sceneCamera.position().native(), 1.0F}};
                sceneGrassClassifyUniformBuffers[currentFrame].update(&classifyData, sizeof(classifyData));
                const GrassPackedStreamUniformData streamData{static_cast<uint32_t>(sceneGpu.grassInstances.size()), 0U, static_cast<uint32_t>(sceneGpu.grassClusters.size()), 0U};
                for (uint32_t stream = 0; stream < 3; ++stream) { auto data = streamData; data.streamIndex = stream; sceneGrassPackedStreamUniformBuffers[currentFrame][stream].update(&data, sizeof(data)); }
                vkCmdFillBuffer(commandBuffer, lists.visibleCount.handle(), 0, VK_WHOLE_SIZE, 0);
                vkCmdFillBuffer(commandBuffer, lists.visibleClusterCount.handle(), 0, VK_WHOLE_SIZE, 0);
                vkCmdFillBuffer(commandBuffer, lists.bladeCullDispatch.handle(), sizeof(uint32_t), sizeof(uint32_t), 0);
                vkCmdFillBuffer(commandBuffer, lists.classifyCounts.handle(), 0, VK_WHOLE_SIZE, 0);
                vkCmdFillBuffer(commandBuffer, lists.binCounts[0].handle(), 0, VK_WHOLE_SIZE, 0); vkCmdFillBuffer(commandBuffer, lists.binCursors[0].handle(), 0, VK_WHOLE_SIZE, 0);
                vkCmdFillBuffer(commandBuffer, lists.mainDrawCount.handle(), 0, sizeof(uint32_t), 0);
                const VkMemoryBarrier2 transferToCompute{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, nullptr, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT};
                const VkDependencyInfo dependency{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &transferToCompute}; vkCmdPipelineBarrier2(commandBuffer, &dependency);
                const auto dispatch = [&](VkPipeline pipeline, VkPipelineLayout layout, VkDescriptorSet set, uint32_t groups) { vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline); vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, nullptr); vkCmdDispatch(commandBuffer, groups, 1, 1); const VkMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, nullptr, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT}; const VkDependencyInfo info{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &barrier}; vkCmdPipelineBarrier2(commandBuffer, &info); };
                dispatch(grassPackedCullPipeline, grassPackedCullPipelineLayout, sceneGrassPackedCullSets[currentFrame], (static_cast<uint32_t>(sceneGpu.grassClusters.size()) + 63U) / 64U);
                const VkMemoryBarrier2 sceneClusterDispatchBarrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, nullptr, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT, VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT}; const VkDependencyInfo sceneClusterDispatchInfo{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &sceneClusterDispatchBarrier}; vkCmdPipelineBarrier2(commandBuffer, &sceneClusterDispatchInfo);
                vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, grassBladeCullPipeline); const VkDescriptorSet sceneBladeSet = sceneGrassBladeCullSets[currentFrame]; vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, grassBladeCullPipelineLayout, 0, 1, &sceneBladeSet, 0, nullptr); vkCmdDispatchIndirect(commandBuffer, lists.bladeCullDispatch.handle(), 0);
                // vkCmdDispatchIndirect is not followed by the helper above;
                // make its visible-blade list available to Scene classify.
                const VkMemoryBarrier2 sceneBladeBarrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, nullptr, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT}; const VkDependencyInfo sceneBladeInfo{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &sceneBladeBarrier}; vkCmdPipelineBarrier2(commandBuffer, &sceneBladeInfo);
                dispatch(grassDispatchBuildPipeline, grassDispatchBuildPipelineLayout, sceneGrassVisibleDispatchBuildSets[currentFrame], 1); const VkMemoryBarrier2 sceneDispatchBuildBarrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, nullptr, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT, VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT}; const VkDependencyInfo sceneDispatchBuildInfo{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &sceneDispatchBuildBarrier}; vkCmdPipelineBarrier2(commandBuffer, &sceneDispatchBuildInfo);
                vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, grassClassifyPipeline); const VkDescriptorSet sceneClassifySet = sceneGrassClassifySets[currentFrame]; vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, grassClassifyPipelineLayout, 0, 1, &sceneClassifySet, 0, nullptr); vkCmdDispatchIndirect(commandBuffer, lists.dispatchIndirect.handle(), 0); vkCmdPipelineBarrier2(commandBuffer, &sceneBladeInfo);
                dispatch(grassDispatchBuildPipeline, grassDispatchBuildPipelineLayout, sceneGrassStreamDispatchBuildSets[currentFrame], 1); vkCmdPipelineBarrier2(commandBuffer, &sceneDispatchBuildInfo);
                // Scene View consumes only the main stream; its shadow and velocity
                // streams are intentionally left unused.
                constexpr uint32_t stream = 0;
                vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, grassPackedBinPipeline); const VkDescriptorSet sceneBinSet = sceneGrassPackedBinSets[currentFrame][stream]; vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, grassPackedBinPipelineLayout, 0, 1, &sceneBinSet, 0, nullptr); vkCmdDispatchIndirect(commandBuffer, lists.dispatchIndirect.handle(), 0); vkCmdPipelineBarrier2(commandBuffer, &sceneBladeInfo);
                dispatch(grassPackedPrefixPipeline, grassPrefixPipelineLayout, sceneGrassPackedPrefixSets[currentFrame][stream], 1);
                vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, grassPackedScatterPipeline); const VkDescriptorSet sceneScatterSet = sceneGrassPackedScatterSets[currentFrame][stream]; vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, grassPackedScatterPipelineLayout, 0, 1, &sceneScatterSet, 0, nullptr); vkCmdDispatchIndirect(commandBuffer, lists.dispatchIndirect.handle(), 0); vkCmdPipelineBarrier2(commandBuffer, &sceneBladeInfo);
                dispatch(grassPackedFinalizePipeline, grassPackedFinalizePipelineLayout, sceneGrassPackedFinalizeSets[currentFrame][stream], (static_cast<uint32_t>(sceneGpu.grassClusters.size()) + 63U) / 64U);
                const VkMemoryBarrier2 grassDrawBarrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, nullptr, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT, VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT};
                const VkDependencyInfo grassDrawDependency{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &grassDrawBarrier};
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
                const std::uint32_t clusterCount = static_cast<std::uint32_t>(
                    std::max<std::size_t>(1, sceneGpu.grassClusters.size()));
                if (pageCount != 0) {
                    grassShadowPageMatricesBuffers[currentFrame].update(pageMatrices.data(),
                        sizeof(Mat4) * pageCount);
                    const GrassShadowPageCullUniformData pageCullData{pageCount, clusterCount, 0, 0};
                    grassShadowPageCullUniformBuffers[currentFrame].update(&pageCullData, sizeof(pageCullData));
                    vkCmdFillBuffer(commandBuffer, grassShadowPageDrawCountBuffers[currentFrame].handle(), 0,
                        sizeof(std::uint32_t) * pageCount, 0);
                    const VkMemoryBarrier2 pageCullPrepare{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, nullptr,
                        VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT};
                    const VkDependencyInfo pageCullPrepareInfo{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                        .memoryBarrierCount = 1, .pMemoryBarriers = &pageCullPrepare};
                    vkCmdPipelineBarrier2(commandBuffer, &pageCullPrepareInfo);
                    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, grassShadowPageCullPipeline);
                    const VkDescriptorSet pageCullSet = grassShadowPageCullSets[currentFrame];
                    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                        grassShadowPageCullPipelineLayout, 0, 1, &pageCullSet, 0, nullptr);
                    vkCmdDispatch(commandBuffer, (clusterCount + 63U) / 64U, pageCount, 1);
                    const VkMemoryBarrier2 pageCullReady{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, nullptr,
                        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                        VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT, VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT};
                    const VkDependencyInfo pageCullReadyInfo{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                        .memoryBarrierCount = 1, .pMemoryBarriers = &pageCullReady};
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
            if (renderGameViewport && mainLightShadows && hadPreviousHiZ &&
                vsmPageMarkingPipeline != VK_NULL_HANDLE &&
                vsmPageMarkingSets[currentFrame] != VK_NULL_HANDLE &&
                vsmPageCompactPipeline != VK_NULL_HANDLE &&
                vsmPageCompactSets[currentFrame] != VK_NULL_HANDLE) {
                vkCmdFillBuffer(commandBuffer, vsmRequestedPageBuffers[currentFrame].handle(),
                                0, VK_WHOLE_SIZE, 0);
                const VkBufferMemoryBarrier2 clearBarrier{
                    .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
                    .srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                    .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                    .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                    .dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                    .buffer = vsmRequestedPageBuffers[currentFrame].handle(),
                    .offset = 0, .size = VK_WHOLE_SIZE};
                const VkDependencyInfo clearDependency{
                    .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                    .bufferMemoryBarrierCount = 1, .pBufferMemoryBarriers = &clearBarrier};
                vkCmdPipelineBarrier2(commandBuffer, &clearDependency);
                gpuTimestampProfiler.beginZone(commandBuffer, currentFrame, shadowPageMarkProfileName);
                vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                                  vsmPageMarkingPipeline);
                const VkDescriptorSet markingSet = vsmPageMarkingSets[currentFrame];
                vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                                        vsmPageMarkingPipelineLayout, 0, 1,
                                        &markingSet, 0, nullptr);
                vkCmdDispatch(commandBuffer, (swapchain.extent().width + 7U) / 8U,
                              (swapchain.extent().height + 7U) / 8U, 1);
                const VkBufferMemoryBarrier2 markingBarrier{
                    .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
                    .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                    .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                    .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                    .dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
                    .buffer = vsmRequestedPageBuffers[currentFrame].handle(),
                    .offset = 0, .size = VK_WHOLE_SIZE};
                const VkDependencyInfo markingDependency{
                    .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                    .bufferMemoryBarrierCount = 1, .pBufferMemoryBarriers = &markingBarrier};
                vkCmdPipelineBarrier2(commandBuffer, &markingDependency);
                gpuTimestampProfiler.endZone(commandBuffer, currentFrame);
                gpuTimestampProfiler.beginZone(commandBuffer, currentFrame, shadowPageCompactProfileName);
                vkCmdFillBuffer(commandBuffer, vsmCompactedPageCountBuffers[currentFrame].handle(),
                                0, sizeof(std::uint32_t), 0);
                const VkBufferMemoryBarrier2 countBarrier{
                    .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
                    .srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                    .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                    .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                    .dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                    .buffer = vsmCompactedPageCountBuffers[currentFrame].handle(),
                    .offset = 0, .size = sizeof(std::uint32_t)};
                const VkDependencyInfo countDependency{
                    .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                    .bufferMemoryBarrierCount = 1, .pBufferMemoryBarriers = &countBarrier};
                vkCmdPipelineBarrier2(commandBuffer, &countDependency);
                vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, vsmPageCompactPipeline);
                const VkDescriptorSet compactSet = vsmPageCompactSets[currentFrame];
                vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                                        vsmPageCompactPipelineLayout, 0, 1, &compactSet, 0, nullptr);
                constexpr std::uint32_t vsmRequestWordCount =
                    (ShadowMap::VirtualPageCount + 31U) / 32U;
                constexpr std::uint32_t vsmCompactionThreadsPerGroup = 64U;
                vkCmdDispatch(commandBuffer,
                              (vsmRequestWordCount + vsmCompactionThreadsPerGroup - 1U) /
                                  vsmCompactionThreadsPerGroup,
                              1, 1);
                const VkBufferMemoryBarrier2 completionBarriers[] = {
                    {.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
                     .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                     .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                     .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT,
                     .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT,
                     .buffer = vsmCompactedPageBuffers[currentFrame].handle(),
                     .offset = 0, .size = VK_WHOLE_SIZE},
                    {.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
                     .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                     .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                     .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT,
                     .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT,
                     .buffer = vsmCompactedPageCountBuffers[currentFrame].handle(),
                     .offset = 0, .size = sizeof(std::uint32_t)}};
                const VkDependencyInfo completionDependency{
                    .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                    .bufferMemoryBarrierCount = std::size(completionBarriers),
                    .pBufferMemoryBarriers = completionBarriers};
                vkCmdPipelineBarrier2(commandBuffer, &completionDependency);
                vsmRequestsReady[currentFrame] = true;
                gpuTimestampProfiler.endZone(commandBuffer, currentFrame);
            }
            gpuTimestampProfiler.beginZone(commandBuffer, currentFrame, shadowProfileName);
            gpuTimestampProfiler.beginZone(commandBuffer, currentFrame, shadowDepthRasterProfileName);
            if (renderGameViewport) {
                // This descriptor set is bound by shadowPass.record(). Update
                // the GTAO image before that first use; Vulkan does not allow
                // updating a bound descriptor set while recording the command
                // buffer unless UPDATE_AFTER_BIND is enabled.
                const VkDescriptorImageInfo gtaoDebugTexture{gtaoPass.debugSampler(gtaoDebugView),
                                                             gtaoPass.debugView(gtaoDebugView),
                                                             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
                shadowPass.setGtaoTexture(currentFrame, gtaoDebugTexture);
                if (vulkanDevice.supportsRayQuery() && !msaa.enabled() &&
                    rtContactShadowSettings.mode == ContactShadowMode::RayTraced &&
                    rtContactShadowPass.resultView() != VK_NULL_HANDLE) {
                    shadowPass.setContactShadowTexture(currentFrame, {
                        rtContactShadowPass.resultSampler(), rtContactShadowPass.resultView(),
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
                }
                shadowPass.setShadowInstanceTransforms(currentFrame,
                    shadowInstanceTransformBuffers[currentFrame].handle(), shadowInstanceMaterialBuffers[currentFrame].handle());
                shadowPass.setShadowInstanceTransforms(currentFrame,
                    shadowTwoSidedInstanceTransformBuffers[currentFrame].handle(), shadowTwoSidedInstanceMaterialBuffers[currentFrame].handle(), true);
                shadowPass.record(
                    commandBuffer, shadowClipMatrices, shadowClipUpdateMask, vertexBuffer.handle(),
                    instanceBuffers[currentFrame].handle(), indexBuffer.handle(),
                    shadowPass.shadowDescriptorSet(currentFrame), shadowPass.shadowTwoSidedDescriptorSet(currentFrame), shadowCullingPasses[currentFrame],
                    shadowIndirectDraws[currentFrame],
                    shadowTwoSidedCullingPasses[currentFrame], shadowTwoSidedIndirectDraws[currentFrame],
                    mainLightShadows
                        ? static_cast<std::uint32_t>(gpuObjects.size()) : 0u,
                    shadowPass.grassShadowDescriptorSet(currentFrame), grassShadowDrawPtr,
                    static_cast<std::uint32_t>(std::max<std::size_t>(1, sceneGpu.grassClusters.size())));
            }

            // Scene View has its own descriptor pass and therefore its own
            // shadow-map image. It must be transitioned as well, even when
            // shadows are disabled, because the forward fragment shader still
            // samples the shadow binding declared by the shared pipeline.
            if (renderSceneViewport) {
                // The Scene View owns a distinct VSM page table. Rebuild the
                // shared scratch ranges only after Game View has consumed its
                // own ranges above.
                if (!sceneGpu.grassInstances.empty()) {
                    const auto pageMatrices = sceneDescriptorPass.grassPageMatrices(sceneShadowClipMatrices);
                    const std::uint32_t pageCount = static_cast<std::uint32_t>(pageMatrices.size());
                    const std::uint32_t clusterCount = static_cast<std::uint32_t>(
                        std::max<std::size_t>(1, sceneGpu.grassClusters.size()));
                    if (pageCount != 0) {
                        grassShadowPageMatricesBuffers[currentFrame].update(pageMatrices.data(), sizeof(Mat4) * pageCount);
                        const GrassShadowPageCullUniformData pageCullData{pageCount, clusterCount, 0, 0};
                        grassShadowPageCullUniformBuffers[currentFrame].update(&pageCullData, sizeof(pageCullData));
                        vkCmdFillBuffer(commandBuffer, grassShadowPageDrawCountBuffers[currentFrame].handle(), 0, sizeof(std::uint32_t) * pageCount, 0);
                        const VkMemoryBarrier2 prepare{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, nullptr, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT};
                        const VkDependencyInfo prepareInfo{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &prepare}; vkCmdPipelineBarrier2(commandBuffer, &prepareInfo);
                        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, grassShadowPageCullPipeline);
                        const VkDescriptorSet pageCullSet = grassShadowPageCullSets[currentFrame];
                        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, grassShadowPageCullPipelineLayout, 0, 1, &pageCullSet, 0, nullptr);
                        vkCmdDispatch(commandBuffer, (clusterCount + 63U) / 64U, pageCount, 1);
                        const VkMemoryBarrier2 ready{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, nullptr, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT, VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT};
                        const VkDependencyInfo readyInfo{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &ready}; vkCmdPipelineBarrier2(commandBuffer, &readyInfo);
                    }
                }
                sceneDescriptorPass.setShadowInstanceTransforms(currentFrame,
                    shadowInstanceTransformBuffers[currentFrame].handle(), shadowInstanceMaterialBuffers[currentFrame].handle());
                sceneDescriptorPass.setShadowInstanceTransforms(currentFrame,
                    shadowTwoSidedInstanceTransformBuffers[currentFrame].handle(), shadowTwoSidedInstanceMaterialBuffers[currentFrame].handle(), true);
                sceneDescriptorPass.record(
                    commandBuffer, sceneShadowClipMatrices, sceneShadowClipUpdateMask,
                    vertexBuffer.handle(), instanceBuffers[currentFrame].handle(), indexBuffer.handle(),
                    sceneDescriptorPass.shadowDescriptorSet(currentFrame), sceneDescriptorPass.shadowTwoSidedDescriptorSet(currentFrame), shadowCullingPasses[currentFrame],
                    shadowIndirectDraws[currentFrame],
                    shadowTwoSidedCullingPasses[currentFrame], shadowTwoSidedIndirectDraws[currentFrame],
                    mainLightShadows
                        ? static_cast<std::uint32_t>(gpuObjects.size()) : 0u,
                    sceneDescriptorPass.grassShadowDescriptorSet(currentFrame), grassShadowDrawPtr,
                    static_cast<std::uint32_t>(std::max<std::size_t>(1, sceneGpu.grassClusters.size())));
            }
            gpuTimestampProfiler.endZone(commandBuffer, currentFrame);
            gpuTimestampProfiler.endZone(commandBuffer, currentFrame);
            gpuTimestampProfiler.beginZone(commandBuffer, currentFrame, cullingProfileName);
            const auto dispatchClusteredLights = [&](VkDescriptorSet set, const VkExtent2D extent) {
                const uint32_t tilesX = (extent.width + ClusterTileSize - 1U) / ClusterTileSize;
                const uint32_t tilesY = (extent.height + ClusterTileSize - 1U) / ClusterTileSize;
                vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, clusteredLightingPipeline);
                vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                                        clusteredLightingPipelineLayout, 0, 1, &set, 0, nullptr);
                vkCmdDispatch(commandBuffer, (tilesX + 3U) / 4U, (tilesY + 3U) / 4U,
                              (ClusterDepthSlices + 3U) / 4U);
                const VkMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, nullptr,
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT};
                const VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO, nullptr, 0, 1, &barrier};
                vkCmdPipelineBarrier2(commandBuffer, &dependency);
            };
            if (renderGameViewport) dispatchClusteredLights(clusteredLightingSets[currentFrame], swapchain.extent());
            if (renderSceneViewport) dispatchClusteredLights(sceneClusteredLightingSets[currentFrame], sceneViewportTarget.extent());
            std::bitset<MaterialProgramSlotCount> activeShaderSlots;
            for (const Culling::GPUObjectData& object : gpuObjects) {
                if (object.shader < MaterialProgramSlotCount && forwardPass.hasMaterialPipeline(object.shader)) {
                    activeShaderSlots.set(object.shader);
                }
            }
            // Step 1 of the frame migration: culling now has an explicit
            // graph node and declared indirect outputs.  It still executes in
            // this command buffer, preserving all existing ordering while the
            // following raster passes are migrated.
            earlyFrameGraph.reset();
            earlyFrameGraph.enablePassCulling();
            earlyFrameGraph.setQueueFamily(RenderGraph::Queue::Graphics, vulkanDevice.graphicsQueueFamily());
            const RenderGraph::BufferDesc indirectDesc{
                .size = indirectBuffers[currentFrame].size(),
                .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT};
            const RenderGraph::BufferDesc drawCountDesc{
                .size = drawCountBuffers[currentFrame].size(),
                .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT};
            const auto graphIndirect = earlyFrameGraph.importBuffer(
                "Main indirect draws", indirectBuffers[currentFrame].handle(), indirectDesc);
            const auto graphDrawCount = earlyFrameGraph.importBuffer(
                "Main draw counts", drawCountBuffers[currentFrame].handle(), drawCountDesc);
            earlyFrameGraph.addPass("GPU culling", RenderGraph::Queue::Graphics,
            [&](RenderGraph::PassBuilder& builder) {
                builder.write(graphIndirect, RenderGraph::BufferUsage::StorageWriteCompute);
                builder.write(graphDrawCount, RenderGraph::BufferUsage::StorageWriteCompute);
            }, [&](const VkCommandBuffer buffer) {
                gpuCullingPasses[currentFrame].recordBinned(
                    buffer, static_cast<std::uint32_t>(gpuObjects.size()), MaterialProgramSlotCount);
            });
            earlyFrameGraph.exportBuffer(graphIndirect);
            earlyFrameGraph.exportBuffer(graphDrawCount);
            earlyFrameGraph.execute(commandBuffer);
            if (renderGameViewport && virtualWaterRenderer.active()) {
                static const ProfileNameId waterPageCullProfileName = Profiler::registerName("Water Page Cull");
                gpuTimestampProfiler.beginZone(commandBuffer, currentFrame, waterPageCullProfileName);
                virtualWaterRenderer.recordCull(commandBuffer, currentFrame, shadowPass.descriptorSet(currentFrame));
                virtualWaterRenderer.recordState(commandBuffer, currentFrame,
                                                 shadowPass.descriptorSet(currentFrame),
                                                 static_cast<float>(Time::deltaTime()));
                gpuTimestampProfiler.endZone(commandBuffer, currentFrame);
            }
            if (renderSceneViewport && sceneVirtualWaterRenderer.active()) {
                static const ProfileNameId sceneWaterPageCullProfileName =
                    Profiler::registerName("Scene Water Page Cull");
                gpuTimestampProfiler.beginZone(commandBuffer, currentFrame, sceneWaterPageCullProfileName);
                sceneVirtualWaterRenderer.recordCull(commandBuffer, currentFrame, sceneDescriptorPass.descriptorSet(currentFrame));
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
            const std::uint32_t grassBinCount = std::max(1u, static_cast<std::uint32_t>(
                sceneGpu.database.meshes().size() * 3u));
            const std::uint32_t visibleCapacity = static_cast<std::uint32_t>(
                sceneGpu.database.instances().size());
            if constexpr (enableExperimentalGpuSceneGrassCompaction) {
                if (visibleCapacity != 0 && cameraController.camera()) {
                const Vec3 cameraPosition = cameraController.camera()->position();
                const GrassIndirectUniformData indirectData{
                    visibleCapacity, visibleCapacity, visibleCapacity, grassBinCount,
                    glm::vec4{cameraPosition.native(), 1.0F}};
                const GrassPrefixUniformData prefixData{grassBinCount, visibleCapacity, 0u, 0u};
                grassIndirectUniformBuffers[currentFrame].update(&indirectData, sizeof(indirectData));
                grassPrefixUniformBuffers[currentFrame].update(&prefixData, sizeof(prefixData));
                vkCmdFillBuffer(commandBuffer, grassBinCountBuffers[currentFrame].handle(), 0,
                                VK_WHOLE_SIZE, 0);
                vkCmdFillBuffer(commandBuffer, grassBinCursorBuffers[currentFrame].handle(), 0,
                                VK_WHOLE_SIZE, 0);
                vkCmdFillBuffer(commandBuffer, grassDrawCountBuffers[currentFrame].handle(), 0,
                                sizeof(std::uint32_t), 0);
                vkCmdFillBuffer(commandBuffer, grassIndirectBuffers[currentFrame].handle(), 0,
                                VK_WHOLE_SIZE, 0);
                const VkMemoryBarrier2 clearBarrier{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                    .srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT, .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                    .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                    .dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT};
                const VkDependencyInfo clearDependency{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                    .memoryBarrierCount = 1, .pMemoryBarriers = &clearBarrier};
                vkCmdPipelineBarrier2(commandBuffer, &clearDependency);
                const auto dispatch = [&](const VkPipeline pipeline, const VkPipelineLayout layout,
                                          const VkDescriptorSet set, const std::uint32_t groups) {
                    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
                    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, nullptr);
                    vkCmdDispatch(commandBuffer, groups, 1, 1);
                    const VkMemoryBarrier2 barrier{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                        .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                        .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                        .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                        .dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT};
                    const VkDependencyInfo dependency{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                        .memoryBarrierCount = 1, .pMemoryBarriers = &barrier};
                    vkCmdPipelineBarrier2(commandBuffer, &dependency);
                };
                dispatch(grassBuildPipeline, grassBuildPipelineLayout, grassBuildSets[currentFrame],
                         (visibleCapacity + 63u) / 64u);
                dispatch(grassPrefixPipeline, grassPrefixPipelineLayout, grassPrefixSets[currentFrame], 1u);
                dispatch(grassScatterPipeline, grassScatterPipelineLayout, grassScatterSets[currentFrame],
                         (visibleCapacity + 63u) / 64u);
                dispatch(grassFinalizePipeline, grassFinalizePipelineLayout, grassFinalizeSets[currentFrame],
                         (grassBinCount + 63u) / 64u);
                const VkMemoryBarrier2 grassDrawBarrier{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                    .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                    .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                    .dstStageMask = VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
                    .dstAccessMask = VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT};
                const VkDependencyInfo grassDrawDependency{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                    .memoryBarrierCount = 1, .pMemoryBarriers = &grassDrawBarrier};
                    vkCmdPipelineBarrier2(commandBuffer, &grassDrawDependency);
                }
            }
            // The first frame has no previous AO yet. Clear it to visibility=1
            // before either Game View or Scene View material shaders sample it.
            gtaoPass.initialize(commandBuffer);
            if (renderGameViewport) {
            viewportFrameGraph.reset();
            viewportFrameGraph.enablePassCulling();
            viewportFrameGraph.setQueueFamily(RenderGraph::Queue::Graphics, vulkanDevice.graphicsQueueFamily());
            viewportFrameGraph.setQueueFamily(RenderGraph::Queue::AsyncCompute, vulkanDevice.computeQueueFamily());
            const VkExtent2D graphExtent = swapchain.extent();
            const RenderGraph::TextureDesc graphDepthDesc{
                .extent = {graphExtent.width, graphExtent.height, 1},
                .format = msaa.enabled() ? hiZDepthBuffer.format() : depthBuffer.format(),
                .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                .aspect = VK_IMAGE_ASPECT_DEPTH_BIT};
            const RenderGraph::TextureDesc graphHdrDesc{
                .extent = {graphExtent.width, graphExtent.height, 1},
                .format = HdrBuffer::Format,
                .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                         VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                .aspect = VK_IMAGE_ASPECT_COLOR_BIT};
            const RenderGraph::TextureState graphHdrInitialState = hdrBufferInitialized
                ? RenderGraph::TextureState{
                    .stage = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                    .access = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                    .layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}
                : RenderGraph::TextureState{
                    .stage = VK_PIPELINE_STAGE_2_NONE,
                    .access = VK_ACCESS_2_NONE,
                    .layout = VK_IMAGE_LAYOUT_UNDEFINED};
            const auto graphPrepassHdr = viewportFrameGraph.importTexture(
                "Forward HDR", hdrBuffer.image(), graphHdrDesc, graphHdrInitialState);
            // The prepass clears this image, so the graph may discard old contents.
            const auto graphDepth = viewportFrameGraph.importTexture(
                "Forward depth", msaa.enabled() ? hiZDepthBuffer.image() : depthBuffer.image(), graphDepthDesc,
                {.layout = VK_IMAGE_LAYOUT_UNDEFINED});
            const RenderGraph::TextureDesc graphVelocityDesc{
                .extent = {graphExtent.width, graphExtent.height, 1},
                .format = VK_FORMAT_R16G16_SFLOAT,
                .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                .aspect = VK_IMAGE_ASPECT_COLOR_BIT};
            const RenderGraph::TextureDesc graphViewNormalDesc{
                .extent = {graphExtent.width, graphExtent.height, 1},
                .format = VK_FORMAT_R16G16_SNORM,
                .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                .aspect = VK_IMAGE_ASPECT_COLOR_BIT};
            const auto graphVelocity = !msaa.enabled()
                ? viewportFrameGraph.importTexture("Forward velocity", velocityBuffer.image(), graphVelocityDesc,
                                                   {.layout = VK_IMAGE_LAYOUT_UNDEFINED})
                : RenderGraph::TextureHandle{};
            const auto graphViewNormal = !msaa.enabled()
                ? viewportFrameGraph.importTexture("GTAO view normals", gtaoViewNormalBuffer.image(), graphViewNormalDesc,
                                                   {.layout = VK_IMAGE_LAYOUT_UNDEFINED})
                : RenderGraph::TextureHandle{};
            const RenderGraph::BufferDesc graphVertexDesc{
                .size = vertexBuffer.size(), .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT};
            const RenderGraph::BufferDesc graphIndexDesc{
                .size = indexBuffer.size(), .usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT};
            const auto graphVertices = viewportFrameGraph.importBuffer(
                "Geometry vertices", vertexBuffer.handle(), graphVertexDesc);
            const auto graphIndices = viewportFrameGraph.importBuffer(
                "Geometry indices", indexBuffer.handle(), graphIndexDesc);
            if (vertexBuffer.readyTimeline() != 0)
                viewportFrameGraph.markUploaded(graphVertices, vertexBuffer.readyTimeline());
            if (indexBuffer.readyTimeline() != 0)
                viewportFrameGraph.markUploaded(graphIndices, indexBuffer.readyTimeline());
            const RenderGraph::BufferDesc foliageIndirectDesc{
                .size = foliageIndirectBuffers[currentFrame].size(),
                .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT};
            const RenderGraph::BufferDesc foliageDrawCountDesc{
                .size = foliageDrawCountBuffers[currentFrame].size(),
                .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT};
            const auto graphFoliageIndirect = viewportFrameGraph.importBuffer(
                "Foliage indirect draws", foliageIndirectBuffers[currentFrame].handle(), foliageIndirectDesc);
            const auto graphFoliageDrawCount = viewportFrameGraph.importBuffer(
                "Foliage draw counts", foliageDrawCountBuffers[currentFrame].handle(), foliageDrawCountDesc);
            const RenderGraph::TextureDesc graphHiZDesc{
                .extent = {hiZBuffer.width(), hiZBuffer.height(), 1}, .format = VK_FORMAT_R32_SFLOAT,
                .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                .aspect = VK_IMAGE_ASPECT_COLOR_BIT, .mipLevels = hiZBuffer.mipCount()};
            const auto graphHiZ = hizEnabled ? viewportFrameGraph.importTexture(
                "Hi-Z pyramid", hiZBuffer.image(), graphHiZDesc,
                {.stage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                 .access = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                 .layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}) : RenderGraph::TextureHandle{};
            viewportFrameGraph.addPass("Foliage GPU culling", RenderGraph::Queue::Graphics,
            [&](RenderGraph::PassBuilder& builder) {
                builder.write(graphFoliageIndirect, RenderGraph::BufferUsage::StorageWriteCompute);
                builder.write(graphFoliageDrawCount, RenderGraph::BufferUsage::StorageWriteCompute);
            }, [&](const VkCommandBuffer buffer) {
                foliageGpuCullingPasses[currentFrame].recordBinned(
                    buffer, static_cast<std::uint32_t>(gpuObjects.size()), MaterialProgramSlotCount);
            });
            viewportFrameGraph.addPass("Depth / prepass", RenderGraph::Queue::Graphics,
            [&](RenderGraph::PassBuilder& builder) {
                builder.write(graphPrepassHdr, RenderGraph::TextureUsage::ColorAttachment);
                builder.write(graphDepth, RenderGraph::TextureUsage::DepthAttachment);
                if (graphVelocity)
                    builder.write(graphVelocity, RenderGraph::TextureUsage::ColorAttachment);
                if (graphViewNormal) {
                    builder.write(graphViewNormal, RenderGraph::TextureUsage::ColorAttachment);
                    builder.setFinalTextureState(graphViewNormal, {
                        .stage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                        .access = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                        .layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                        .write = false});
                }
                builder.read(graphVertices, RenderGraph::BufferUsage::VertexRead);
                builder.read(graphIndices, RenderGraph::BufferUsage::IndexRead);
                builder.read(graphFoliageIndirect, RenderGraph::BufferUsage::IndirectRead);
                builder.read(graphFoliageDrawCount, RenderGraph::BufferUsage::IndirectRead);
                builder.setFinalTextureState(graphDepth, {
                    .stage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                             VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                             VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                    .access = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT |
                              VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT,
                    .layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
                    .write = false});
            }, [&](const VkCommandBuffer buffer) {
                gpuTimestampProfiler.endZone(buffer, currentFrame);
                gpuTimestampProfiler.beginZone(buffer, currentFrame, forwardProfileName);
                forwardPass.begin(buffer, msaa.enabled() ? msaa.colorImageView() : hdrBuffer.imageView(),
                    depthBuffer.imageView(), msaa.enabled() ? VK_NULL_HANDLE : velocityBuffer.imageView(),
                    msaa.enabled() ? VK_NULL_HANDLE : gtaoViewNormalBuffer.imageView(),
                    msaa.enabled() ? hdrBuffer.imageView() : VK_NULL_HANDLE,
                    msaa.enabled() ? hiZDepthBuffer.imageView() : VK_NULL_HANDLE, swapchain.extent(), shadowPass.descriptorSet(currentFrame),
                    vertexBuffer.handle(), instanceBuffers[currentFrame].handle(), indexBuffer.handle());
                for (std::uint32_t shader = 0; shader < MaterialProgramSlotCount; ++shader) {
                    if (!activeShaderSlots.test(shader) || shader == materialShaderIndex(MaterialShader::Water)) continue;
                    const auto commandOffset = static_cast<VkDeviceSize>(shader) * gpuObjects.size() * sizeof(VkDrawIndexedIndirectCommand);
                    const auto countOffset = static_cast<VkDeviceSize>(shader) * sizeof(std::uint32_t);
                    forwardPass.drawMaterial(buffer, shadowPass.descriptorSet(currentFrame), shader, indirectDraws[currentFrame], commandOffset, countOffset);
                    forwardPass.drawMaterial(buffer, shadowPass.descriptorSet(currentFrame), shader, foliageIndirectDraws[currentFrame], commandOffset, countOffset);
                }
                if (!sceneGpu.grassInstances.empty()) {
                    const auto& lists = grassRenderLists[currentFrame]; Culling::IndexedIndirectDrawCount grassDraw;
                    grassDraw.create(lists.mainIndirect.handle(), lists.mainDrawCount.handle(), static_cast<uint32_t>(std::max<std::size_t>(1, sceneGpu.grassClusters.size())));
                    shadowPass.setGrassVisibleInstances(currentFrame, lists.drawInstances[0].handle());
                    forwardPass.drawGrass(buffer, shadowPass.grassDescriptorSet(currentFrame), grassDraw);
                }
                ForwardPass::end(buffer);
                gpuTimestampProfiler.endZone(buffer, currentFrame);
            });
            if (hizEnabled) {
                viewportFrameGraph.addPass("Hi-Z", RenderGraph::Queue::Graphics,
                [&](RenderGraph::PassBuilder& builder) {
                    builder.read(graphDepth, RenderGraph::TextureUsage::SampledReadCompute);
                    builder.write(graphHiZ, RenderGraph::TextureUsage::StorageWriteCompute);
                }, [this](const VkCommandBuffer buffer) {
                    hiZPasses[currentFrame].record(buffer, hiZBuffers[currentFrame]);
                });
                viewportFrameGraph.exportTexture(graphHiZ);
            } else viewportFrameGraph.exportTexture(graphDepth);
            // These callbacks record the prepass and Hi-Z at their declared
            // position.  Do not defer graph execution until the end of the
            // viewport: GTAO and the lighting pass below consume this depth.
            viewportFrameGraph.execute(commandBuffer);
            hdrBufferInitialized = true;
            if (hizEnabled) {
                hiZViewProjections[currentFrame] = cameraController.camera()->projectionMatrix().native() *
                                                  cameraController.camera()->viewMatrix().native();
                hiZValid[currentFrame] = true;
            }

            // The prepass has produced this frame's depth and velocity. GTAO
            // must finish before the lighting pass samples its result.
            const DepthBuffer& gtaoDepth = msaa.enabled() ? hiZDepthBuffer : depthBuffer;
            const Mat4 inverseProjection{glm::inverse(cameraController.camera()->projectionMatrix().native())};
            // The rotating GTAO pattern is only stable when TAA accumulates
            // it.  Without TAA, hold it fixed instead of producing per-frame
            // AO shimmer.
            gtaoPass.record(commandBuffer, currentFrame,
                            taaResolveActive ? static_cast<std::uint32_t>(submittedFrameValue) : 0U,
                            gtaoDepth.imageView(), gtaoDepth.sampler(),
                            msaa.enabled() ? gtaoDepth.imageView() : gtaoViewNormalBuffer.imageView(),
                            msaa.enabled() ? gtaoDepth.sampler() : gtaoViewNormalBuffer.sampler(), !msaa.enabled(), inverseProjection);

            const bool rtContactRequested = vulkanDevice.supportsRayQuery() && !msaa.enabled() &&
                rtContactShadowSettings.mode == ContactShadowMode::RayTraced;
            if (rtContactRequested && rayTracingBlasDirty && vertexBuffer.hasDeviceAddress() && indexBuffer.hasDeviceAddress()) {
                std::vector<AccelerationStructureManager::MeshBuildInput> meshes;
                meshes.reserve(geometryHeapAllocations.size());
                for (const auto& [key, allocation] : geometryHeapAllocations) {
                    meshes.push_back({key,
                        vertexBuffer.deviceAddress() + VkDeviceAddress(allocation.firstVertex) * sizeof(GpuVertex),
                        indexBuffer.deviceAddress() + VkDeviceAddress(allocation.firstIndex) * sizeof(std::uint32_t),
                        allocation.vertexCount, allocation.indexCount});
                }
                accelerationStructures.rebuildBlases(commandBuffer, meshes);
                rayTracingBlasDirty = false;
                rtTlasInputDirty = true;
            }
            if (rtContactRequested && !rayTracingBlasDirty) {
                const std::uint64_t transformRevision = registry.componentRevision<Transform>();
                const std::uint64_t topologyRevision = registry.renderTopologyRevision();
                const bool inputChanged = rtTlasInputDirty ||
                    transformRevision != lastRtTlasTransformRevision ||
                    topologyRevision != lastRtTlasTopologyRevision;
                if (inputChanged) {
                    rtTlasInstances.clear();
                    for (const InstanceBatch& batch : instanceBatches) {
                        if (batch.twoSided || batch.mesh == nullptr) continue;
                        for (std::uint32_t offset = 0; offset < batch.instanceCount; ++offset) {
                            const RendererInstanceData& source = instanceModels[batch.firstInstance + offset];
                            const glm::quat q{source.rotation.w, source.rotation.x, source.rotation.y, source.rotation.z};
                            glm::mat4 model = glm::mat4_cast(q);
                            model[0] *= source.scaleBase.x; model[1] *= source.scaleBase.y; model[2] *= source.scaleBase.z;
                            model[3] = glm::vec4(source.positionMaterial.x, source.positionMaterial.y, source.positionMaterial.z, 1.0F);
                            AccelerationStructureManager::InstanceBuildInput input{};
                            input.meshKey = batch.mesh; input.mask = 0x01; input.customIndex = batch.firstInstance + offset;
                            for (std::uint32_t row = 0; row < 3; ++row)
                                for (std::uint32_t column = 0; column < 4; ++column)
                                    input.transform[row * 4 + column] = model[column][row];
                            rtTlasInstances.push_back(input);
                        }
                    }
                    lastRtTlasTransformRevision = transformRevision;
                    lastRtTlasTopologyRevision = topologyRevision;
                    rtTlasInputDirty = false;
                }
                if (inputChanged || !accelerationStructures.built(currentFrame)) {
                    gpuTimestampProfiler.beginZone(commandBuffer, currentFrame, rtTlasProfileName);
                    accelerationStructures.updateTlas(commandBuffer, currentFrame, rtTlasInstances);
                    gpuTimestampProfiler.endZone(commandBuffer, currentFrame);
                }
                if (accelerationStructures.built(currentFrame)) {
                    gpuTimestampProfiler.beginZone(commandBuffer, currentFrame, rtContactProfileName);
                    rtContactShadowPass.record(commandBuffer, currentFrame, accelerationStructures.tlas(currentFrame),
                        depthBuffer.imageView(), depthBuffer.sampler(), gtaoViewNormalBuffer.imageView(),
                        gtaoViewNormalBuffer.sampler(), rtContactShadowSettings);
                    gpuTimestampProfiler.endZone(commandBuffer, currentFrame);
                }
            }

            gpuTimestampProfiler.beginZone(commandBuffer, currentFrame, forwardProfileName);
            gpuTimestampProfiler.beginZone(commandBuffer, currentFrame, shadowProjectionProfileName);
            lightingForwardPass.begin(commandBuffer, msaa.enabled() ? msaa.colorImageView() : hdrBuffer.imageView(),
                depthBuffer.imageView(), antialiasingLevel == AntialiasingLevel::TAA ? velocityBuffer.imageView() : VK_NULL_HANDLE,
                VK_NULL_HANDLE, msaa.enabled() ? hdrBuffer.imageView() : VK_NULL_HANDLE,
                msaa.enabled() ? hiZDepthBuffer.imageView() : VK_NULL_HANDLE, swapchain.extent(),
                shadowPass.descriptorSet(currentFrame), vertexBuffer.handle(), instanceBuffers[currentFrame].handle(), indexBuffer.handle());
            for (std::uint32_t shader = 0; shader < MaterialProgramSlotCount; ++shader) {
                if (!activeShaderSlots.test(shader)) continue;
                if (shader == materialShaderIndex(MaterialShader::Water)) continue;
                const auto commandOffset = static_cast<VkDeviceSize>(shader) * gpuObjects.size() * sizeof(VkDrawIndexedIndirectCommand);
                const auto countOffset = static_cast<VkDeviceSize>(shader) * sizeof(std::uint32_t);
                lightingForwardPass.drawMaterial(commandBuffer, shadowPass.descriptorSet(currentFrame), shader, indirectDraws[currentFrame], commandOffset, countOffset);
                lightingForwardPass.drawMaterial(commandBuffer, shadowPass.descriptorSet(currentFrame), shader, foliageIndirectDraws[currentFrame], commandOffset, countOffset);
            }
            if (!sceneGpu.grassInstances.empty()) {
                const auto& lists = grassRenderLists[currentFrame]; Culling::IndexedIndirectDrawCount grassDraw;
                grassDraw.create(lists.mainIndirect.handle(), lists.mainDrawCount.handle(), static_cast<uint32_t>(std::max<std::size_t>(1, sceneGpu.grassClusters.size())));
                shadowPass.setGrassVisibleInstances(currentFrame, lists.drawInstances[0].handle());
                lightingForwardPass.drawGrass(commandBuffer, shadowPass.grassDescriptorSet(currentFrame), grassDraw);
            }
            gpuTimestampProfiler.endZone(commandBuffer, currentFrame);
            skyPass.record(commandBuffer, currentFrame);
            // Water must be composited before alpha-blended particles.  A
            // particle does not write depth, so rendering it into the opaque
            // source would let the later water pass incorrectly cover it.
            const bool hasLegacyWater = activeShaderSlots.test(materialShaderIndex(MaterialShader::Water));
            const bool hasVirtualWater = virtualWaterRenderer.active();
            const bool needsWaterComposite = hasLegacyWater || hasVirtualWater;
            if (!needsWaterComposite) {
                if (particleSystem && cameraController.camera()) {
                    const Particles::ParticleFrameData particleFrame{
                        cameraController.camera()->projectionMatrix() * cameraController.camera()->viewMatrix(),
                        particlePreviousGameViewProjection,
                        cameraController.camera()->right(), 0.0F, cameraController.camera()->up(), 0.0F,
                        particlePreviousGameCameraRight, 0.0F, particlePreviousGameCameraUp, 0.0F,
                        static_cast<float>(Time::deltaTime())};
                    particleSystem->recordRender(commandBuffer, particleFrame, particlePipeline.handle(),
                                                 particlePipeline.layout(), currentFrame, false);
                }
            }
            // This pipeline was created against LightingForwardPass's render
            // pass. It must be recorded while that pass is active, not in
            // WaterPass, whose external dependency also reads the color
            // attachment and is therefore render-pass incompatible.
            lightingForwardPass.drawOutline(commandBuffer, shadowPass.descriptorSet(currentFrame), indirectDraws[currentFrame]);
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
                const VkDependencyInfo postLightingDependency{
                    .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                    .imageMemoryBarrierCount = antialiasingLevel == AntialiasingLevel::TAA ? 2U : 1U,
                    .pImageMemoryBarriers = postLightingTransitions.data()};
                vkCmdPipelineBarrier2(commandBuffer, &postLightingDependency);
            }
            if (needsWaterComposite) {
                const VkImageMemoryBarrier2 barriersBefore[] = {
                    {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, nullptr,
                     VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                     VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, hdrBuffer.image(),
                     {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}},
                    {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, nullptr,
                     opaqueSceneColorInitialized ? VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT : VK_PIPELINE_STAGE_2_NONE,
                     opaqueSceneColorInitialized ? VK_ACCESS_2_SHADER_SAMPLED_READ_BIT : 0,
                     VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                     opaqueSceneColorInitialized ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
                     opaqueSceneColor.image(), {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}},
                };
                VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
                dependency.imageMemoryBarrierCount = std::size(barriersBefore);
                dependency.pImageMemoryBarriers = barriersBefore;
                vkCmdPipelineBarrier2(commandBuffer, &dependency);
                const VkImageCopy copy{{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0},
                                       {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0},
                                       {swapchain.extent().width, swapchain.extent().height, 1}};
                vkCmdCopyImage(commandBuffer, hdrBuffer.image(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               opaqueSceneColor.image(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
                const VkImageMemoryBarrier2 barriersAfter[] = {
                    {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, nullptr,
                     VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                     VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT |
                         VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                     VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, hdrBuffer.image(),
                     {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}},
                    {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, nullptr,
                     VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                     VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, opaqueSceneColor.image(),
                     {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}},
                };
                dependency.imageMemoryBarrierCount = std::size(barriersAfter);
                dependency.pImageMemoryBarriers = barriersAfter;
                vkCmdPipelineBarrier2(commandBuffer, &dependency);
                opaqueSceneColorInitialized = true;
                const auto commandOffset = static_cast<VkDeviceSize>(materialShaderIndex(MaterialShader::Water)) *
                    gpuObjects.size() * sizeof(VkDrawIndexedIndirectCommand);
                const auto countOffset = static_cast<VkDeviceSize>(materialShaderIndex(MaterialShader::Water)) *
                    sizeof(std::uint32_t);
                if (hasVirtualWater) {
                    if (cameraController.camera()) {
                        virtualWaterRenderer.updateUnderwaterState(
                            currentFrame, registry, cameraController.camera()->position(),
                            static_cast<float>(Time::elapsedTime()));
                    }
                    static const ProfileNameId waterPrepassProfileName = Profiler::registerName("Virtual Water Prepass");
                    static const ProfileNameId waterShadeProfileName = Profiler::registerName("Virtual Water Shading");
                    gpuTimestampProfiler.beginZone(commandBuffer, currentFrame, waterPrepassProfileName);
                    virtualWaterRenderer.recordPrepass(commandBuffer, currentFrame,
                        shadowPass.descriptorSet(currentFrame), vertexBuffer.handle(), indexBuffer.handle(),
                        indirectDraws[currentFrame], commandOffset, countOffset);
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
                waterColorTransition.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT |
                                                       VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
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
                waterPass.begin(commandBuffer,
                                msaa.enabled() ? msaa.colorImageView() : hdrBuffer.imageView(),
                                depthBuffer.imageView(),
                                velocityBuffer.imageView(),
                                msaa.enabled() ? hdrBuffer.imageView() : VK_NULL_HANDLE,
                                msaa.enabled() ? hiZDepthBuffer.imageView() : VK_NULL_HANDLE,
                                swapchain.extent(),
                                shadowPass.descriptorSet(currentFrame), currentFrame,
                                vertexBuffer.handle(), indexBuffer.handle());
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
                    const Particles::ParticleFrameData particleFrame{
                        cameraController.camera()->projectionMatrix() * cameraController.camera()->viewMatrix(),
                        particlePreviousGameViewProjection,
                        cameraController.camera()->right(), 0.0F, cameraController.camera()->up(), 0.0F,
                        particlePreviousGameCameraRight, 0.0F, particlePreviousGameCameraUp, 0.0F,
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
            }

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
                depthToAttachment.srcStageMask = sceneViewportDepthInitialized
                    ? VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT : VK_PIPELINE_STAGE_2_NONE;
                depthToAttachment.srcAccessMask = sceneViewportDepthInitialized
                    ? VK_ACCESS_2_SHADER_SAMPLED_READ_BIT : 0;
                depthToAttachment.dstStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                                                 VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
                depthToAttachment.dstAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                                                  VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
                depthToAttachment.oldLayout = sceneViewportDepthInitialized
                    ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
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

                sceneForwardPass.begin(
                    commandBuffer, sceneViewportTarget.color().imageView(), sceneViewportDepthBuffer.imageView(),
                    VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE, sceneViewportTarget.extent(),
                    sceneDescriptorPass.descriptorSet(currentFrame), vertexBuffer.handle(),
                    instanceBuffers[currentFrame].handle(), indexBuffer.handle());
                for (std::uint32_t shader = 0; shader < MaterialProgramSlotCount; ++shader) {
                    if (!activeShaderSlots.test(shader)) continue;
                    // Scene View will receive the same dedicated post-opaque
                    // WaterPass when its viewport render graph is split. Do
                    // not accidentally run water through its opaque pass.
                    if (shader == materialShaderIndex(MaterialShader::Water)) continue;
                    const auto commandOffset = static_cast<VkDeviceSize>(shader) * gpuObjects.size() *
                        sizeof(VkDrawIndexedIndirectCommand);
                    const auto countOffset = static_cast<VkDeviceSize>(shader) * sizeof(std::uint32_t);
                    sceneForwardPass.drawMaterial(commandBuffer, sceneDescriptorPass.descriptorSet(currentFrame),
                        shader, sceneIndirectDraws[currentFrame], commandOffset, countOffset);
                    sceneForwardPass.drawMaterial(commandBuffer, sceneDescriptorPass.descriptorSet(currentFrame),
                        shader, sceneFoliageIndirectDraws[currentFrame], commandOffset, countOffset);
                }
                if (!sceneGpu.grassInstances.empty()) {
                    const auto& lists = sceneGrassRenderLists[currentFrame];
                    Culling::IndexedIndirectDrawCount grassDraw;
                    grassDraw.create(lists.mainIndirect.handle(), lists.mainDrawCount.handle(),
                                     static_cast<uint32_t>(std::max<std::size_t>(1, sceneGpu.grassClusters.size())));
                    sceneDescriptorPass.setGrassVisibleInstances(currentFrame, lists.drawInstances[0].handle());
                    sceneForwardPass.drawGrass(commandBuffer, sceneDescriptorPass.grassDescriptorSet(currentFrame), grassDraw);
                }
                sceneSkyPass.record(commandBuffer, currentFrame);
                if (particleSystem) {
                    Camera sceneCamera{Degrees{60.0F},
                                       static_cast<float>(sceneViewportTarget.extent().width) /
                                           static_cast<float>(sceneViewportTarget.extent().height),
                                       0.1F, 1000.0F};
                    sceneCamera.setPosition(cameraController.editorPosition());
                    sceneCamera.setRotation(Degrees{cameraController.editorYaw()},
                                            Degrees{cameraController.editorPitch()});
                    const Particles::ParticleFrameData particleFrame{
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
                    particleSystem->recordRender(commandBuffer, particleFrame,
                                                 sceneParticlePipeline.handle(), sceneParticlePipeline.layout(),
                                                 currentFrame, true);
                }
                sceneForwardPass.drawOutline(commandBuffer, sceneDescriptorPass.descriptorSet(currentFrame),
                                        sceneIndirectDraws[currentFrame]);
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
                depthToSampled.srcStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                                                VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
                depthToSampled.srcAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
                depthToSampled.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
                depthToSampled.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
                depthToSampled.oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
                depthToSampled.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
                depthToSampled.image = sceneViewportDepthBuffer.image();
                depthToSampled.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
                const VkImageMemoryBarrier2 sceneToSampled[] = {colorToSampled, depthToSampled};
                VkDependencyInfo sceneToSampledDependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
                sceneToSampledDependency.imageMemoryBarrierCount = std::size(sceneToSampled);
                sceneToSampledDependency.pImageMemoryBarriers = sceneToSampled;
                vkCmdPipelineBarrier2(commandBuffer, &sceneToSampledDependency);
                sceneViewportDepthInitialized = true;

                const bool hasSceneLegacyWater = activeShaderSlots.test(materialShaderIndex(MaterialShader::Water));
                const bool hasSceneVirtualWater = sceneVirtualWaterRenderer.active();
                if (hasSceneLegacyWater || hasSceneVirtualWater) {
                    const VkImageMemoryBarrier2 beforeCopy[] = {
                        {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, nullptr,
                         VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                         VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, sceneViewportTarget.color().image(),
                         {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}},
                        {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, nullptr,
                         sceneOpaqueColorInitialized ? VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT : VK_PIPELINE_STAGE_2_NONE,
                         sceneOpaqueColorInitialized ? VK_ACCESS_2_SHADER_SAMPLED_READ_BIT : 0,
                         VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                         sceneOpaqueColorInitialized ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
                         sceneOpaqueColor.image(), {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}},
                    };
                    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
                    dependency.imageMemoryBarrierCount = std::size(beforeCopy);
                    dependency.pImageMemoryBarriers = beforeCopy;
                    vkCmdPipelineBarrier2(commandBuffer, &dependency);
                    const VkExtent2D extent = sceneViewportTarget.extent();
                    const VkImageCopy copy{{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0},
                                           {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0},
                                           {extent.width, extent.height, 1}};
                    vkCmdCopyImage(commandBuffer, sceneViewportTarget.color().image(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                   sceneOpaqueColor.image(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
                    const VkImageMemoryBarrier2 afterCopy[] = {
                        {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, nullptr,
                         VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                         VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                         VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                         VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, sceneViewportTarget.color().image(),
                         {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}},
                        {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, nullptr,
                         VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                         VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                         VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, sceneOpaqueColor.image(),
                         {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}},
                    };
                    dependency.imageMemoryBarrierCount = std::size(afterCopy);
                    dependency.pImageMemoryBarriers = afterCopy;
                    vkCmdPipelineBarrier2(commandBuffer, &dependency);
                    sceneOpaqueColorInitialized = true;

                    const auto commandOffset = static_cast<VkDeviceSize>(materialShaderIndex(MaterialShader::Water)) *
                        gpuObjects.size() * sizeof(VkDrawIndexedIndirectCommand);
                    const auto countOffset = static_cast<VkDeviceSize>(materialShaderIndex(MaterialShader::Water)) *
                        sizeof(std::uint32_t);
                    static const ProfileNameId sceneWaterPrepassProfileName =
                        Profiler::registerName("Scene Virtual Water Prepass");
                    static const ProfileNameId sceneWaterShadeProfileName =
                        Profiler::registerName("Scene Virtual Water Shading");
                    gpuTimestampProfiler.beginZone(commandBuffer, currentFrame, sceneWaterPrepassProfileName);
                    sceneVirtualWaterRenderer.recordPrepass(commandBuffer, currentFrame,
                        sceneDescriptorPass.descriptorSet(currentFrame), vertexBuffer.handle(), indexBuffer.handle(),
                        sceneIndirectDraws[currentFrame], commandOffset, countOffset);
                    gpuTimestampProfiler.endZone(commandBuffer, currentFrame);
                    gpuTimestampProfiler.beginZone(commandBuffer, currentFrame, sceneWaterShadeProfileName);
                    sceneVirtualWaterRenderer.recordAdaptiveShading(commandBuffer, currentFrame,
                        sceneDescriptorPass.descriptorSet(currentFrame));
                    sceneVirtualWaterRenderer.recordComposite(commandBuffer, currentFrame,
                        sceneDescriptorPass.descriptorSet(currentFrame));
                    gpuTimestampProfiler.endZone(commandBuffer, currentFrame);
                }
            }

                });

            // Depth / Hi-Z is recorded by the graph directly after the
            // prepass. This legacy submit split remains here temporarily for
            // the other async command-buffer infrastructure, but must never
            // record the pyramid a second time.
            if (false && renderGameViewport) {
                // Hi-Z is a complete graph resource: Forward produces the
                // imported depth image and this pass writes the imported mip
                // chain.  The graph owns the outer depth/read -> storage/write
                // transition; HiZPass owns only per-mip dependencies.
                frameGraph.reset();
                frameGraph.enablePassCulling();
                const auto& hiZBuffer = hiZBuffers[currentFrame];
                const VkExtent2D extent = swapchain.extent();
                const RenderGraph::TextureDesc depthDesc{
                    .extent = {extent.width, extent.height, 1},
                    .format = msaa.enabled() ? hiZDepthBuffer.format() : depthBuffer.format(),
                    .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                    .aspect = VK_IMAGE_ASPECT_DEPTH_BIT};
                const auto depth = frameGraph.importTexture(
                    "Forward depth", msaa.enabled() ? hiZDepthBuffer.image() : depthBuffer.image(), depthDesc,
                    {.stage = VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                     .access = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                     .layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
                     .write = true});
                const RenderGraph::TextureDesc hiZDesc{
                    .extent = {hiZBuffer.width(), hiZBuffer.height(), 1},
                    .format = VK_FORMAT_R32_SFLOAT,
                    .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                    .aspect = VK_IMAGE_ASPECT_COLOR_BIT,
                    .mipLevels = hiZBuffer.mipCount()};
                const auto hiZ = frameGraph.importTexture(
                    "Hi-Z pyramid", hiZBuffer.image(), hiZDesc,
                    {.stage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                     .access = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                     .layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
                const bool canSubmitHiZAsync = vulkanDevice.hasAsyncComputeQueue() &&
                    vulkanDevice.computeQueueFamily() == vulkanDevice.graphicsQueueFamily();
                frameGraph.setQueueFamily(RenderGraph::Queue::Graphics, vulkanDevice.graphicsQueueFamily());
                frameGraph.setQueueFamily(RenderGraph::Queue::AsyncCompute, vulkanDevice.computeQueueFamily());
                frameGraph.addPass("Hi-Z", canSubmitHiZAsync ? RenderGraph::Queue::AsyncCompute : RenderGraph::Queue::Graphics,
                [&](RenderGraph::PassBuilder& builder) {
                    builder.read(depth, RenderGraph::TextureUsage::SampledReadCompute);
                    builder.write(hiZ, RenderGraph::TextureUsage::StorageWriteCompute);
                }, [this](const VkCommandBuffer buffer) {
                    hiZPasses[currentFrame].record(buffer, hiZBuffers[currentFrame]);
                });
                // This image is sampled by culling and VSM page marking on the
                // next use of this frame slot, so it is the graph's external
                // output even though Present is produced later in the frame.
                frameGraph.exportTexture(hiZ);
                // Depth is produced outside this graph by Forward. Until that
                // producer is graph-owned too, async Hi-Z is restricted to a
                // same-family compute queue; RenderGraph already handles
                // ownership release/acquire for graph-to-graph edges.
                if (canSubmitHiZAsync) {
                    // Close the producer batch now. The remaining post work
                    // is recorded in a second graphics command buffer and can
                    // overlap the compute queue after the first batch signals.
                    if (vkEndCommandBuffer(commandBuffer) != VK_SUCCESS) {
                        throw std::runtime_error("Could not end graphics producer command buffer for async Hi-Z");
                    }
                    VkCommandBuffer asyncBuffer = asyncComputeCommandBuffers.at(currentFrame);
                    vkResetCommandBuffer(asyncBuffer, 0);
                    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
                    if (vkBeginCommandBuffer(asyncBuffer, &begin) != VK_SUCCESS) {
                        throw std::runtime_error("Could not begin async Hi-Z command buffer");
                    }
                    frameGraph.execute(RenderGraph::Queue::AsyncCompute, asyncBuffer);
                    if (vkEndCommandBuffer(asyncBuffer) != VK_SUCCESS) {
                        throw std::runtime_error("Could not end async Hi-Z command buffer");
                    }
                    commandBuffer = postAsyncGraphicsCommandBuffers.at(currentFrame);
                    vkResetCommandBuffer(commandBuffer, 0);
                    if (vkBeginCommandBuffer(commandBuffer, &begin) != VK_SUCCESS) {
                        throw std::runtime_error("Could not begin post-async graphics command buffer");
                    }
                    asyncHiZSubmittedThisFrame = true;
                } else {
                    frameGraph.execute(commandBuffer);
                }
                hiZValid[currentFrame] = true;
            }

            // Presentation is one declarative chain.  Keeping TAA, bloom and
            // the final raster passes in a single graph gives the compiler the
            // actual HDR-history and swapchain hazards instead of relying on
            // the render-pass layout transitions hidden inside their callbacks.
            frameGraph.setQueueFamily(RenderGraph::Queue::Graphics,
                                                  vulkanDevice.graphicsQueueFamily());
            const VkExtent2D postExtent = swapchain.extent();
            const RenderGraph::TextureDesc hdrDesc{
                .extent = {postExtent.width, postExtent.height, 1}, .format = HdrBuffer::Format,
                .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                .aspect = VK_IMAGE_ASPECT_COLOR_BIT};
            const RenderGraph::TextureDesc velocityDesc{
                .extent = {postExtent.width, postExtent.height, 1}, .format = VK_FORMAT_R16G16_SFLOAT,
                .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                .aspect = VK_IMAGE_ASPECT_COLOR_BIT};
            const auto graphHdr = frameGraph.importTexture(
                "Game HDR", hdrBuffer.image(), hdrDesc,
                {.stage = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                 .access = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                 .layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
            const auto graphVelocity = frameGraph.importTexture(
                "Game velocity", velocityBuffer.image(), velocityDesc,
                {.stage = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                 .access = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                 .layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
            const DepthBuffer& taaDepth = msaa.enabled() ? hiZDepthBuffer : depthBuffer;
            const RenderGraph::TextureDesc depthDesc{
                .extent = {postExtent.width, postExtent.height, 1}, .format = taaDepth.format(),
                .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                .aspect = VK_IMAGE_ASPECT_DEPTH_BIT};
            const auto graphDepth = frameGraph.importTexture(
                "Game depth", taaDepth.image(), depthDesc,
                {.stage = VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                 .access = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                 .layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL, .write = true});
            const auto graphBloom = frameGraph.importTexture(
                "Bloom", bloomPass.resultImage(), hdrDesc,
                {.stage = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                 .access = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                 .layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
            const RenderGraph::TextureDesc presentDesc{
                .extent = {postExtent.width, postExtent.height, 1}, .format = swapchain.format(),
                .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, .aspect = VK_IMAGE_ASPECT_COLOR_BIT};
            const auto graphPresent = frameGraph.importTexture(
                "Swapchain", swapchain.images().at(imageIndex.value), presentDesc,
                {.layout = VK_IMAGE_LAYOUT_UNDEFINED});

            RenderGraph::TextureHandle graphPostSource = graphHdr;
            if (renderGameViewport && taaResolveActive) {
                // Initialize before importing: from here the render graph is
                // the sole owner of the color-history image layouts.
                temporalAaPass.prepareHistory(commandBuffer);
                const std::uint32_t historyReadIndex = temporalAaPass.resolvedIndex();
                const std::uint32_t historyWriteIndex = temporalAaPass.nextResolvedIndex();
                const auto graphHistoryRead = frameGraph.importTexture(
                    "TAA history read", temporalAaPass.historyImage(historyReadIndex), hdrDesc,
                    {.stage = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                     .access = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                     .layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
                const auto graphHistoryWrite = frameGraph.importTexture(
                    "TAA history write", temporalAaPass.historyImage(historyWriteIndex), hdrDesc,
                    {.stage = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                     .access = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                     .layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
                RenderGraph::TextureHandle graphWaterVelocity;
                RenderGraph::TextureHandle graphWaterMeta;
                RenderGraph::TextureHandle graphWaterSurface;
                if (virtualWaterPreparedThisFrame) {
                    graphWaterVelocity = frameGraph.importTexture(
                        "Water velocity", virtualWaterRenderer.velocityImage(), hdrDesc,
                        {.stage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                         .access = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                         .layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, .write = true});
                    graphWaterMeta = frameGraph.importTexture(
                        "Water metadata", virtualWaterRenderer.metaImage(), hdrDesc,
                        {.stage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                         .access = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                         .layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, .write = true});
                    graphWaterSurface = frameGraph.importTexture(
                        "Water surface", virtualWaterRenderer.surfaceImage(), hdrDesc,
                        {.stage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                         .access = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                         .layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, .write = true});
                }
                frameGraph.addPass("TAA resolve", RenderGraph::Queue::Graphics,
                [&](RenderGraph::PassBuilder& builder) {
                    builder.read(graphHdr, RenderGraph::TextureUsage::SampledReadFragment);
                    builder.read(graphVelocity, RenderGraph::TextureUsage::SampledReadFragment);
                    builder.read(graphDepth, RenderGraph::TextureUsage::DepthReadFragment);
                    builder.read(graphHistoryRead, RenderGraph::TextureUsage::SampledReadFragment);
                    if (graphWaterVelocity) {
                        builder.read(graphWaterVelocity, RenderGraph::TextureUsage::SampledReadFragment);
                        builder.read(graphWaterMeta, RenderGraph::TextureUsage::SampledReadFragment);
                        builder.read(graphWaterSurface, RenderGraph::TextureUsage::SampledReadFragment);
                    }
                    builder.write(graphHistoryWrite, RenderGraph::TextureUsage::ColorAttachment);
                    builder.setFinalTextureState(graphHistoryWrite, {
                        .stage = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                        .access = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                        .layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, .write = false});
                }, [&](const VkCommandBuffer buffer) {
                    gpuTimestampProfiler.beginZone(buffer, currentFrame, taaProfileName);
                    temporalAaPass.setVirtualWaterEnabled(virtualWaterPreparedThisFrame);
                    temporalAaPass.record(buffer, postExtent, taaJitterX, taaJitterY);
                    gpuTimestampProfiler.endZone(buffer, currentFrame);
                });
                graphPostSource = graphHistoryWrite;
            } else {
                frameGraph.addPass("TAA disabled", RenderGraph::Queue::Graphics,
                    [](RenderGraph::PassBuilder&) {}, [&](const VkCommandBuffer buffer) {
                        gpuTimestampProfiler.beginZone(buffer, currentFrame, taaProfileName);
                        gpuTimestampProfiler.endZone(buffer, currentFrame);
                    });
            }
            if (renderGameViewport) {
                frameGraph.addPass("Bloom", RenderGraph::Queue::Graphics,
                [&](RenderGraph::PassBuilder& builder) {
                    builder.read(graphPostSource, RenderGraph::TextureUsage::SampledReadFragment);
                    builder.write(graphBloom, RenderGraph::TextureUsage::ColorAttachment);
                    builder.setFinalTextureState(graphBloom, {
                        .stage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                        .access = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                        .layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, .write = true});
                }, [&](const VkCommandBuffer buffer) {
                    gpuTimestampProfiler.beginZone(buffer, currentFrame, bloomProfileName);
                    bloomPass.record(buffer, taaResolveActive ? temporalAaPass.resolvedView() : hdrBuffer.imageView(),
                                     hdrBuffer.sampler(), currentFrame);
                    gpuTimestampProfiler.endZone(buffer, currentFrame);
                });
            }
            frameGraph.addPass(editorUiActive ? "Editor UI" : "Tonemap and UI",
                                          RenderGraph::Queue::Graphics,
            [&](RenderGraph::PassBuilder& builder) {
                if (editorUiActive) {
                    // The editor samples the Game View descriptor. Make
                    // that external shader read visible to the graph so its
                    // producer (notably TAA resolve) is not dead-pass culled.
                    if (renderGameViewport)
                        builder.read(graphPostSource, RenderGraph::TextureUsage::SampledReadFragment);
                } else {
                    builder.read(graphPostSource, RenderGraph::TextureUsage::SampledReadFragment);
                    if (renderGameViewport) builder.read(graphBloom, RenderGraph::TextureUsage::SampledReadFragment);
                }
                builder.write(graphPresent, RenderGraph::TextureUsage::ColorAttachment);
                builder.setFinalTextureState(graphPresent, {
                    .stage = VK_PIPELINE_STAGE_2_NONE, .access = VK_ACCESS_2_NONE,
                    .layout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, .write = true});
            }, [&](const VkCommandBuffer buffer) {
                gpuTimestampProfiler.beginZone(buffer, currentFrame, tonemapProfileName);
                if (editorUiActive) {
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
                    if (editorUiBackend) editorUiBackend->renderDrawData(reinterpret_cast<std::uint64_t>(buffer));
                    vkCmdEndRendering(buffer);
                } else {
                    if (taaResolveActive)
                        tonemapPass.record(buffer, imageIndex.value, postExtent, 0.0F,
                                           1U + temporalAaPass.resolvedIndex());
                    else tonemapPass.record(buffer, imageIndex.value, postExtent);
                    canvasRenderer.record(scene.uiCanvas(), buffer,
                        UI::CanvasRenderer::ImageIndex{imageIndex.value},
                        UI::CanvasRenderer::FrameIndex{currentFrame}, postExtent);
                }
                gpuTimestampProfiler.endZone(buffer, currentFrame);
            });
            frameGraph.exportTexture(graphPresent);
            frameGraph.execute(commandBuffer);
            gpuTimestampProfiler.endFrame(commandBuffer, currentFrame);

            if (vkEndCommandBuffer(commandBuffer) != VK_SUCCESS) {
                throw std::runtime_error("Could not end command buffer");
            }
        }

        void createSyncObjects() {
            imageAvailableSemaphores.resize(MAX_FRAMES_IN_FLIGHT);
            renderFinishedSemaphores.resize(swapchain.imageCount());
            inFlightFences.resize(MAX_FRAMES_IN_FLIGHT);

            VkSemaphoreCreateInfo semaphoreInfo{};
            semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

            if (vulkanDevice.hasAsyncComputeQueue()) {
                VkSemaphoreTypeCreateInfo timelineInfo{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
                timelineInfo.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
                semaphoreInfo.pNext = &timelineInfo;
                if (vkCreateSemaphore(device, &semaphoreInfo, nullptr, &asyncComputeTimeline) != VK_SUCCESS) {
                    throw std::runtime_error("Could not create async compute timeline semaphore");
                }
                semaphoreInfo.pNext = nullptr;
            }

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

            // Shadow/scene descriptor sets reference the packed grass
            // buffers owned by culling resources. They cannot outlive a
            // culling resize/rebuild.
            virtualWaterRenderer.destroy();
            forwardPass.destroy();
            lightingForwardPass.destroy();
            waterPass.destroy();
            shadowPass.destroy();
            sceneDescriptorPass.destroy();
            destroyCullingResources();

            canvasRenderer.destroy();
            tonemapPass.destroy();
            temporalAaPass.destroy();
            bloomPass.destroy();
            gtaoPass.destroy();
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
            hdrBuffer.create(vulkanDevice.physical(), device, swapchain.extent(), vulkanDevice.allocator());
            hdrBufferInitialized = false;
            opaqueSceneColor.create(vulkanDevice.physical(), device, swapchain.extent(), vulkanDevice.allocator());
            opaqueSceneColorInitialized = false;
            msaa.create(swapchain.extent(), HdrBuffer::Format);
            createDepthResources();
            createGtaoPass();
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
            // In the editor, Scene View and Game View are mutually exclusive.
            // Do not skip this update merely because the Scene View controller
            // is off: Play Mode still needs to enter SDL relative mouse mode.
            if (editorUiActive && !cameraController.editorInputEnabled() &&
                !cameraController.gameInputEnabled()) {
                // update() is also responsible for leaving SDL relative mouse
                // mode.  It must run once when Play Mode is stopped; otherwise
                // a cursor captured in the preceding frame remains locked.
                cameraController.update(window, registry);
                return;
            }
            cameraController.update(window, registry);
        }

        void updateEditorSceneCameraInput() {
            if (!cameraController.editorInputEnabled()) return;
            const Vec3 beforePosition = cameraController.editorPosition();
            const float beforeYaw = cameraController.editorYaw();
            const float beforePitch = cameraController.editorPitch();
            cameraController.updateEditor(window);
            if (beforePosition.x() != cameraController.editorPosition().x() ||
                beforePosition.y() != cameraController.editorPosition().y() ||
                beforePosition.z() != cameraController.editorPosition().z() ||
                beforeYaw != cameraController.editorYaw() ||
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
            if (colliderIterator == particleColliderIndices.end()) { return; }
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
            const bool rebuild = particleColliderRegistry != &registry ||
                particleColliderStructuralRevision != structuralRevision;
            bool cacheChanged = false;

            if (rebuild) {
                rebuildParticleColliderCache();
                cacheChanged = true;
            } else if (particleColliderComponentRevision != colliderRevision ||
                       particleColliderTransformRevision != transformRevision) {
                std::unordered_set<Entity> changed;
                const auto addChanged = [&](const auto& entities) {
                    changed.insert(entities.begin(), entities.end());
                };
                addChanged(registry.componentEntitiesChangedSince<ColliderComponent>(
                    particleColliderComponentRevision));
                addChanged(registry.componentEntitiesChangedSince<Transform>(
                    particleColliderTransformRevision));

                const Registry& readRegistry = registry;
                for (const Entity entity : changed) {
                    if (!readRegistry.valid(entity) ||
                        !readRegistry.has<ColliderComponent>(entity) ||
                        !readRegistry.has<Transform>(entity)) {
                        removeParticleCollider(entity);
                        cacheChanged = true;
                        continue;
                    }
                    const Particles::ParticleCollider value = RendererSceneHelpers::makeParticleCollider(
                        readRegistry.get<ColliderComponent>(entity),
                        readRegistry.get<Transform>(entity));
                    if (const auto colliderIterator = particleColliderIndices.find(entity);
                        colliderIterator != particleColliderIndices.end()) {
                        cachedParticleColliders[colliderIterator->second] = value;
                    } else {
                        particleColliderIndices.emplace(entity, cachedParticleColliders.size());
                        cachedParticleColliders.push_back(value);
                        particleColliderEntities.push_back(entity);
                    }
                    cacheChanged = true;
                }
            }

            particleColliderRegistry = &registry;
            particleColliderStructuralRevision = structuralRevision;
            particleColliderComponentRevision = colliderRevision;
            particleColliderTransformRevision = transformRevision;
            return cacheChanged;
        }

        [[nodiscard]] bool acquireFrameImage(uint32_t& imageIndex) {
            vkWaitForFences(device, 1, &inFlightFences[currentFrame], VK_TRUE, UINT64_MAX);
            // Every buffer retired into this slot belonged to the submission
            // guarded by this fence, so destruction is now safe.
            deferredPreviousTransformBuffers[currentFrame].clear();
            completedFrameValue = std::max(completedFrameValue,
                                           frameSubmissionValues[currentFrame]);
            gpuRetirementQueue.collect(completedFrameValue);
            sceneGpu.database.reclaimDeferredInstances(completedFrameValue);
            if (const auto completed = gpuTimestampProfiler.completedFrame(currentFrame)) {
                lastGpuProfile = *completed;
                float gameWaterMs = 0.0F;
                float sceneWaterMs = 0.0F;
                for (const GpuProfileEvent& event : completed->events) {
                    const std::string_view name = Profiler::name(event.name);
                    const float duration = std::max(event.endMs - event.startMs, 0.0F);
                    if (name == "Water Page Cull" || name == "Virtual Water Prepass" ||
                        name == "Virtual Water Shading") gameWaterMs += duration;
                    else if (name == "Scene Water Page Cull" || name == "Scene Virtual Water Prepass" ||
                             name == "Scene Virtual Water Shading") sceneWaterMs += duration;
                }
                virtualWaterRenderer.onFrameCompleted(currentFrame, gameWaterMs);
                sceneVirtualWaterRenderer.onFrameCompleted(currentFrame, sceneWaterMs);
            } else {
                virtualWaterRenderer.onFrameCompleted();
                sceneVirtualWaterRenderer.onFrameCompleted();
            }
            const VkResult result = vkAcquireNextImageKHR(device, swapchain.handle(), UINT64_MAX,
                imageAvailableSemaphores[currentFrame], VK_NULL_HANDLE, &imageIndex);

            if (result == VK_ERROR_OUT_OF_DATE_KHR) {
                recreateSwapChain();
                return false;
            } if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
                throw std::runtime_error("Could not acquire swap chain image");
            }

            vkResetFences(device, 1, &inFlightFences[currentFrame]);
            return true;
        }

        void updateParticleSystemForFrame() {
            if (!particleSystem) { return; }

            const Entity particleEntity = scene.particleEntity();
            const bool hasEmitter = particleEntity != NullEntity &&
                (registry.has<ParticleEmitterComponent>(particleEntity) ||
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
            VkSemaphore signalSemaphores[] = {renderFinishedSemaphores[imageIndex]};
            // Do not derive this from UploadContext::lastSubmittedValue(): a
            // transfer unrelated to this frame must not stall graphics.  The
            // authoritative frame graph supplies only imported resources that
            // have an actual consumer in this frame.
            std::uint64_t uploadValue = 0;
            VkPipelineStageFlags2 uploadStage = VK_PIPELINE_STAGE_2_NONE;
            for (const RenderGraph::UploadWait& wait : frameGraph.uploadWaits()) {
                if (wait.queue != RenderGraph::Queue::Graphics) continue;
                uploadValue = std::max(uploadValue, wait.timelineValue);
                uploadStage |= wait.stage;
            }
            const bool waitForUploads = uploadValue != 0;
            if (uploadStage == VK_PIPELINE_STAGE_2_NONE) uploadStage = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            if (asyncHiZSubmittedThisFrame) {
                const std::uint64_t graphicsValue = ++asyncComputeTimelineValue;
                const std::uint64_t computeValue = ++asyncComputeTimelineValue;
                const VkCommandBuffer graphicsBuffer = commandBuffers[currentFrame];
                const VkCommandBuffer postGraphicsBuffer = postAsyncGraphicsCommandBuffers.at(currentFrame);
                const VkCommandBuffer computeBuffer = asyncComputeCommandBuffers.at(currentFrame);
                const VkSemaphoreSubmitInfo imageAvailable{
                    .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
                    .semaphore = imageAvailableSemaphores[currentFrame],
                    .stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT};
                const VkSemaphoreSubmitInfo uploadWait{
                    .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
                    .semaphore = uploadContext.timeline(), .value = uploadValue,
                    .stageMask = uploadStage};
                const std::array graphicsWaits = {imageAvailable, uploadWait};
                const VkCommandBufferSubmitInfo graphicsCommand{
                    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = graphicsBuffer};
                const VkSemaphoreSubmitInfo graphicsSignal{
                    .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = asyncComputeTimeline,
                    .value = graphicsValue, .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT};
                const VkSubmitInfo2 graphicsSubmit{
                    .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
                    .waitSemaphoreInfoCount = waitForUploads ? 2u : 1u,
                    .pWaitSemaphoreInfos = graphicsWaits.data(),
                    .commandBufferInfoCount = 1, .pCommandBufferInfos = &graphicsCommand,
                    .signalSemaphoreInfoCount = 1, .pSignalSemaphoreInfos = &graphicsSignal};
                if (vkQueueSubmit2(vulkanDevice.graphicsQueue(), 1, &graphicsSubmit, VK_NULL_HANDLE) != VK_SUCCESS) {
                    throw std::runtime_error("Could not submit graphics batch before async Hi-Z");
                }

                const VkSemaphoreSubmitInfo computeWait{
                    .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = asyncComputeTimeline,
                    .value = graphicsValue, .stageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT};
                const VkCommandBufferSubmitInfo computeCommand{
                    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = computeBuffer};
                const VkSemaphoreSubmitInfo computeSignal{
                    .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = asyncComputeTimeline,
                    // asyncComputeCommandBuffers contain only the graph's Hi-Z
                    // compute pass.  Signal once its shader writes are
                    // available instead of serialising unrelated stages.
                    .value = computeValue, .stageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT};
                const VkSubmitInfo2 computeSubmit{
                    .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
                    .waitSemaphoreInfoCount = 1, .pWaitSemaphoreInfos = &computeWait,
                    .commandBufferInfoCount = 1, .pCommandBufferInfos = &computeCommand,
                    .signalSemaphoreInfoCount = 1, .pSignalSemaphoreInfos = &computeSignal};
                if (vkQueueSubmit2(vulkanDevice.computeQueue(), 1, &computeSubmit, VK_NULL_HANDLE) != VK_SUCCESS) {
                    throw std::runtime_error("Could not submit async Hi-Z command buffer");
                }

                const VkCommandBufferSubmitInfo postGraphicsCommand{
                    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = postGraphicsBuffer};
                // Presentation depends on the graphics work which writes the
                // swapchain image, not on Hi-Z.  Hi-Z is an exported frame-slot
                // resource consumed when this slot is used again.
                const VkSemaphoreSubmitInfo renderFinished{
                    .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = signalSemaphores[0],
                    .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT};
                const VkSubmitInfo2 postGraphicsSubmit{
                    .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
                    .commandBufferInfoCount = 1, .pCommandBufferInfos = &postGraphicsCommand,
                    .signalSemaphoreInfoCount = 1, .pSignalSemaphoreInfos = &renderFinished};
                if (vkQueueSubmit2(vulkanDevice.graphicsQueue(), 1, &postGraphicsSubmit, VK_NULL_HANDLE) != VK_SUCCESS) {
                    throw std::runtime_error("Could not submit graphics batch overlapping async Hi-Z");
                }

                // Keep the frame-slot fence behind both queues: the graphics
                // queue processes this after postGraphicsSubmit, while the
                // timeline wait keeps Hi-Z alive until it can be reused.
                const VkSemaphoreSubmitInfo completeWait{
                    .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = asyncComputeTimeline,
                    .value = computeValue, .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT};
                const VkSubmitInfo2 completionSubmit{
                    .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
                    .waitSemaphoreInfoCount = 1, .pWaitSemaphoreInfos = &completeWait};
                if (vkQueueSubmit2(vulkanDevice.graphicsQueue(), 1, &completionSubmit,
                                   inFlightFences[currentFrame]) != VK_SUCCESS) {
                    throw std::runtime_error("Could not complete async Hi-Z submission chain");
                }
            } else {
            VkSubmitInfo submitInfo{};
            submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;

            VkSemaphore waitSemaphores[] = {imageAvailableSemaphores[currentFrame], uploadContext.timeline()};
            VkPipelineStageFlags waitStages[] = {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                static_cast<VkPipelineStageFlags>(uploadStage)};
            std::uint64_t waitValues[] = {0, uploadValue};
            VkTimelineSemaphoreSubmitInfo timelineInfo{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
            if (waitForUploads) {
                timelineInfo.waitSemaphoreValueCount = 2;
                timelineInfo.pWaitSemaphoreValues = waitValues;
                submitInfo.pNext = &timelineInfo;
            }
            submitInfo.waitSemaphoreCount = waitForUploads ? 2u : 1u;
            submitInfo.pWaitSemaphores = waitSemaphores;
            submitInfo.pWaitDstStageMask = waitStages;
            submitInfo.commandBufferCount = 1;
            submitInfo.pCommandBuffers = &commandBuffers[currentFrame];

            submitInfo.signalSemaphoreCount = 1;
            submitInfo.pSignalSemaphores = signalSemaphores;

            const VkResult submitResult = vkQueueSubmit(
                vulkanDevice.graphicsQueue(), 1, &submitInfo, inFlightFences[currentFrame]);
            if (submitResult != VK_SUCCESS) {
                throw std::runtime_error(
                    "Could not submit command buffer to queue (VkResult " +
                    std::to_string(static_cast<int>(submitResult)) + ")");
            }
            }
            frameSubmissionValues[currentFrame] = ++submittedFrameValue;
            VkPresentInfoKHR presentInfo{};
            presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
            presentInfo.waitSemaphoreCount = 1;
            presentInfo.pWaitSemaphores = signalSemaphores;

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
            if (!hasDrawableExtent()) { return; }
            uint32_t imageIndex;
            if (!acquireFrameImage(imageIndex)) { return; }

            vkResetCommandBuffer(commandBuffers[currentFrame], 0);
            vkResetCommandBuffer(postAsyncGraphicsCommandBuffers[currentFrame], 0);
            asyncHiZSubmittedThisFrame = false;
            {
                GE_PROFILE_SCOPE("Refresh Scene Data");
                refreshSceneFrameData();
            }
            {
                GE_PROFILE_SCOPE("GPU Object Update");
                updateRenderableBuffers();
            }
            const Vec3 sceneCameraPosition = cameraController.editorPosition();
            const float sceneCameraYaw = cameraController.editorYaw();
            const float sceneCameraPitch = cameraController.editorPitch();
            const bool sceneCameraChanged = !sceneViewportCacheValid ||
                sceneCameraPosition.x() != renderedSceneViewportPosition.x() ||
                sceneCameraPosition.y() != renderedSceneViewportPosition.y() ||
                sceneCameraPosition.z() != renderedSceneViewportPosition.z() ||
                sceneCameraYaw != renderedSceneViewportYaw ||
                sceneCameraPitch != renderedSceneViewportPitch;
            // Both paths retain the Scene View result until the camera, scene,
            // viewport, or explicit editor state requests a redraw. The direct
            // path uses a pass whose input and output layouts are sampled.
            // Water animation is time-dependent, therefore a cached Scene
            // View cannot remain valid while virtual water is active.
            sceneViewportRendered = sceneViewportActive &&
                (sceneViewportNeedsRender || sceneCameraChanged ||
                 scene.mutationRevision() != sceneViewportRenderedRevision ||
                 sceneVirtualWaterRenderer.active());
            const bool renderGameViewport = !editorUiActive || !sceneViewportActive;
            if (renderGameViewport != gameShadowContextActive) {
                shadowPass.invalidateCache();
                sceneDescriptorPass.invalidateCache();
                shadowClipmapsValid = false;
                sceneShadowClipmapsValid = false;
                gameShadowContextActive = renderGameViewport;
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
            createRtContactShadowPass();
            {
                GE_PROFILE_SCOPE("Command Recording");
                recordCommandBuffer(commandBuffers[currentFrame], SwapchainImageIndex{imageIndex});
            }
            {
                GE_PROFILE_SCOPE("Queue Submit / Present");
                submitAndPresentFrame(imageIndex);
            }
            // Only this path records timestamp queries.  drawCoreFrame() shares
            // submitAndPresentFrame(), but does not reset or write this pool.
            gpuTimestampProfiler.markSubmitted(currentFrame, Profiler::currentFrameNumber());
            if (sceneViewportRendered) {
                renderedSceneViewportPosition = sceneCameraPosition;
                renderedSceneViewportYaw = sceneCameraYaw;
                renderedSceneViewportPitch = sceneCameraPitch;
                sceneViewportRenderedRevision = scene.mutationRevision();
                sceneViewportNeedsRender = false;
                sceneViewportCacheValid = true;
                sceneViewportImageInitialized = true;
            }
            if (sceneViewportImageInitialized && sceneViewportDescriptor == VK_NULL_HANDLE) {
                if (editorUiBackend) sceneViewportDescriptor = reinterpret_cast<VkDescriptorSet>(editorUiBackend->addTexture(
                    reinterpret_cast<std::uint64_t>(sceneViewportTarget.color().imageView()),
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL));
            }

            currentFrame = (currentFrame + 1) % MAX_FRAMES_IN_FLIGHT;
            ++shadowClipFrameIndex;
        }

        void drawCoreFrame() {
            if (!hasDrawableExtent()) return;
            uint32_t imageIndex;
            if (!acquireFrameImage(imageIndex)) return;
            asyncHiZSubmittedThisFrame = false;
            VkCommandBuffer commandBuffer = commandBuffers[currentFrame];
            vkResetCommandBuffer(commandBuffer, 0);
            VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            if (vkBeginCommandBuffer(commandBuffer, &beginInfo) != VK_SUCCESS) {
                throw std::runtime_error("Could not begin core command buffer");
            }
            VkImageMemoryBarrier2 toColor{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
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
            if (editorUiBackend) editorUiBackend->renderDrawData(reinterpret_cast<std::uint64_t>(commandBuffer));
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
                snprintf(title, sizeof(title),
                         "GamEngine | FPS: %.1F | Renderables: %zu",
                         fps, renderables.size());
                SDL_SetWindowTitle(window, title);

                fpsFrameCount = 0;
                fpsElapsedTime = 0.0;
            }
        }

        // ---------- CLEANUP ----------
