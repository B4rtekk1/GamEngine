        struct DirectionalLight final {
            static constexpr float defaultIntensity{4.0F};
            Vec3 direction{-0.45F, -0.80F, -0.35F};
            Math::Color color = Math::Color::white();
            float intensity{defaultIntensity};
        };

        [[nodiscard]] DirectionalLight directionalLight() const {
            DirectionalLight result;
            const Registry& readRegistry = registry;
            bool found = false;
            readRegistry.view<Transform, LightComponent>(
                [&](const Entity entity, const Transform& transform, const LightComponent& light) {
                    // SceneEditor guarantees that only one directional
                    // LightComponent is enabled. The guard keeps the runtime
                    // path deterministic even for externally constructed scenes.
                    if (found || !light.enabled || light.type != LightType::Directional) return;
                    const glm::mat4 world = transform.worldMatrix().native();
                    const glm::vec3 direction = glm::vec3(world *
                                                          glm::vec4{0.0F, 0.0F, -1.0F, 0.0F});
                    if (glm::length(direction) <= 1e-6F) return;
                    result.direction = Vec3{glm::normalize(direction)};
                    result.color = readRegistry.has<ColorPickerComponent>(entity)
                                       ? readRegistry.get<ColorPickerComponent>(entity).color
                                       : light.color;
                    result.intensity = std::max(0.0F, light.intensity);
                    found = true;
                });
            return result;
        }

        struct WindFrameData final {
            Vec4 directionStrength{};
            Vec4 sourcePositionRange{};
            Vec4 gustFrequencyTime{};
        };

        [[nodiscard]] WindFrameData windFrameData() const {
            WindFrameData result;
            const Registry& readRegistry = registry;
            bool found = false;
            readRegistry.view<Transform, WindComponent>([&](const Entity, const Transform& transform,
                                                             const WindComponent& wind) {
                if (found || !wind.enabled || wind.strength <= 0.0F || wind.range <= 0.0F) return;
                const float length = wind.direction.length();
                if (length <= 1.0e-4F) return;
                const float now = static_cast<float>(Time::elapsedTime());
                const Vec3 direction = wind.direction * (1.0F / length);
                result.directionStrength = Vec4{direction.x(), direction.y(), direction.z(), wind.strength};
                const glm::vec3 position = glm::vec3{transform.worldMatrix().native()[3]};
                result.sourcePositionRange = Vec4{position.x, position.y, position.z, wind.range};
                result.gustFrequencyTime = Vec4{wind.gustStrength, wind.frequency, now,
                                                 now - static_cast<float>(Time::deltaTime())};
                found = true;
            });
            return result;
        }

        [[nodiscard]] std::pair<std::array<LocalLightGPU, MaxLocalLights>, std::uint32_t>
        localLights() const {
            std::array<LocalLightGPU, MaxLocalLights> result{};
            std::uint32_t count{};
            const Registry& readRegistry = registry;
            readRegistry.view<Transform, LightComponent>(
                [&](const Entity entity, const Transform& transform, const LightComponent& light) {
                    if (!light.enabled || light.type == LightType::Directional ||
                        count == MaxLocalLights || light.range <= 0.0F) return;
                    const glm::mat4 world = transform.worldMatrix().native();
                    const glm::vec3 direction = glm::vec3(world *
                                                          glm::vec4{0.0F, 0.0F, -1.0F, 0.0F});
                    const Math::Color color = readRegistry.has<ColorPickerComponent>(entity)
                                                  ? readRegistry.get<ColorPickerComponent>(entity).color
                                                  : light.color;
                    constexpr float pi = 3.14159265358979323846F;
                    const float outerRadians = light.outerConeAngle * pi / 180.0F;
                    const float innerRadians = light.innerConeAngle * pi / 180.0F;
                    auto& gpu = result[count++];
                    const glm::vec3 position = glm::vec3{world[3]};
                    gpu.positionRange = {position.x, position.y, position.z, light.range};
                    gpu.directionOuterCos = {glm::normalize(direction), std::cos(outerRadians)};
                    gpu.colorIntensity = {color.r(), color.g(), color.b(), std::max(0.0F, light.intensity)};
                    gpu.parameters = {std::cos(innerRadians), static_cast<float>(light.type),
                                      light.castShadows ? 1.0F : 0.0F, 0.0F};
                });
            return {result, count};
        }

        void updateUniformBuffer(const uint32_t frame) {
            Entity activeCamera = NullEntity;
            const Registry& readRegistry = registry;
            readRegistry.view<CameraComponent, Transform>(
                [&](const Entity entity, const CameraComponent& component, const Transform&) {
                    if (activeCamera == NullEntity && component.primary && component.isPerspective() &&
                        component.isValid()) {
                        activeCamera = entity;
                    }
                });
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
                const auto& component = readRegistry.get<CameraComponent>(activeCamera);

                // Game View is presented in a fixed 16:9 editor frame. Keep the
                // projection in that aspect too, independently of dock layout.
                const float gameAspect = editorUiActive ? (16.0F / 9.0F) : component.aspectRatio;
                cameraController.camera().emplace(Degrees{component.fieldOfView}, gameAspect,
                                                   component.nearClip, component.farClip);
                // Cameras can be parented to a player or another scene object.
                // Use the composed transform rather than the camera's local
                // Transform, otherwise a child camera is rendered at its local
                // origin (usually inside the terrain).
                const glm::mat4 model = worldModel(activeCamera);
                // A camera's pitch is a view-space angle, not merely an X-axis
                // model rotation. Deriving it from the model matrix made pitch
                // depend on yaw, producing an unnatural vertical mouse look.
                const Transform &cameraTransform = readRegistry.get<Transform>(activeCamera);
                float yaw = cameraTransform.rotation.y();
                float pitch = cameraTransform.rotation.x();
                std::unordered_map<UUID, Entity> entitiesByUuid;
                entitiesByUuid.reserve(readRegistry.size());
                readRegistry.view<UUIDComponent>([&](const Entity entity, const UUIDComponent &uuid) {
                    entitiesByUuid.emplace(uuid.value, entity);
                });
                Entity current = activeCamera;
                while (readRegistry.has<ParentComponent>(current)) {
                    const UUID parentUuid = readRegistry.get<ParentComponent>(current).parent;
                    const auto parent = entitiesByUuid.find(parentUuid);
                    if (parent == entitiesByUuid.end() || !readRegistry.has<Transform>(parent->second)) break;
                    current = parent->second;
                    const Transform &parentTransform = readRegistry.get<Transform>(current);
                    yaw += parentTransform.rotation.y();
                    pitch += parentTransform.rotation.x();
                }
                cameraController.camera()->setPosition(Vec3{glm::vec3{model[3]}});
                cameraController.camera()->setRotation(Degrees{yaw}, Degrees{pitch});
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
                taaJitterX = (halton(taaSampleIndex + 1, 2) - 0.5F) * 2.0F /
                             static_cast<float>(extent.width);
                taaJitterY = (halton(taaSampleIndex + 1, 3) - 0.5F) * 2.0F /
                             static_cast<float>(extent.height);
                ++taaSampleIndex;
                cameraController.camera()->setProjectionJitter(taaJitterX, taaJitterY);
            }

            const DirectionalLight light = directionalLight();
            const WindFrameData wind = windFrameData();
            const auto [lights, localLightCount] = localLights();
            shadowClipUpdateMask = updateVirtualShadowClipmaps(
                cameraController.camera()->position(), shadowClipMatrices,
                lastShadowCameraPosition, lastShadowLightDirection, shadowClipmapsValid);
            if (optimizationFeatures.shadows && hasShadowCasters) {
                shadowPass.preparePages(
                    shadowClipMatrices,
                    cameraController.camera()->projectionMatrix() *
                        cameraController.camera()->viewMatrix(),
                    gpuObjects, dirtyShadowObjects, currentFrame);
            } else {
                shadowPass.invalidateCache();
            }
            const Mat4 currentView = cameraController.camera()->viewMatrix();
            const Mat4 currentProjection = cameraController.camera()->projectionMatrix();
            const Vec3 currentCameraPosition = cameraController.camera()->position();
            const Vec3 currentCameraForward = cameraController.camera()->forward();
            // Motion vectors describe continuous motion.  Reusing history after
            // a teleport or a large orientation jump produces unavoidable
            // ghosting, so treat it as a camera cut instead.
            constexpr float cameraCutDistance = 5.0F;
            constexpr float cameraCutDirectionDot = 0.8660254F; // 30 degrees
            const bool cameraCut = previousGameCameraValid &&
                ((currentCameraPosition - previousGameCameraPosition).length() > cameraCutDistance ||
                 dot(currentCameraForward, previousGameCameraForward) < cameraCutDirectionDot);
            if (taaResolveActive && cameraCut) {
                temporalAaPass.reset();
            }
            const UniformBufferObject data{
                currentView, currentProjection,
                previousGameCameraValid ? previousGameView : currentView,
                previousGameCameraValid ? previousGameProjection : currentProjection,
                shadowClipMatrices,
                Vec4{cameraController.camera()->position().x(), cameraController.camera()->position().y(),
                     cameraController.camera()->position().z(), 1.0F},
                Vec4{light.direction.x(), light.direction.y(), light.direction.z(), light.intensity},
                Vec4{light.color.r(), light.color.g(), light.color.b(), 1.0F},
                wind.directionStrength, wind.sourcePositionRange, wind.gustFrequencyTime,
                (optimizationFeatures.shadows && hasShadowCasters) ? 1u : 0u,
                materialSlots, editorSelectedRenderable, 0u, localLightCount, lights};
            uniformBuffers[frame].update(&data, sizeof(data));
            previousGameView = currentView;
            previousGameProjection = currentProjection;
            previousGameCameraPosition = currentCameraPosition;
            previousGameCameraForward = currentCameraForward;
            previousGameCameraValid = true;
        }

        void updateSceneViewportUniformBuffer(const uint32_t frame) {
            const float aspect = static_cast<float>(sceneViewportTarget.extent().width) /
                                 static_cast<float>(sceneViewportTarget.extent().height);
            Camera sceneCamera{Degrees{60.0F}, aspect, 0.1F, 1000.0F};
            sceneCamera.setPosition(cameraController.editorPosition());
            sceneCamera.setRotation(Degrees{cameraController.editorYaw()},
                                    Degrees{cameraController.editorPitch()});
            const DirectionalLight light = directionalLight();
            const WindFrameData wind = windFrameData();
            const auto [lights, localLightCount] = localLights();
            sceneShadowClipUpdateMask = updateVirtualShadowClipmaps(
                sceneCamera.position(), sceneShadowClipMatrices,
                lastSceneShadowCameraPosition, lastSceneShadowLightDirection,
                sceneShadowClipmapsValid);
            if (optimizationFeatures.shadows && hasShadowCasters) {
                sceneDescriptorPass.preparePages(
                    sceneShadowClipMatrices,
                    sceneCamera.projectionMatrix() * sceneCamera.viewMatrix(),
                    gpuObjects, dirtyShadowObjects, currentFrame);
            } else {
                sceneDescriptorPass.invalidateCache();
            }
            const Mat4 sceneView = sceneCamera.viewMatrix();
            const Mat4 sceneProjection = sceneCamera.projectionMatrix();
            const UniformBufferObject data{
                sceneView, sceneProjection, sceneView, sceneProjection,
                sceneShadowClipMatrices,
                Vec4{sceneCamera.position().x(), sceneCamera.position().y(), sceneCamera.position().z(), 1.0F},
                Vec4{light.direction.x(), light.direction.y(), light.direction.z(), light.intensity},
                Vec4{light.color.r(), light.color.g(), light.color.b(), 1.0F},
                wind.directionStrength, wind.sourcePositionRange, wind.gustFrequencyTime,
                (optimizationFeatures.shadows && hasShadowCasters) ? 1u : 0u,
                materialSlots, editorSelectedRenderable, 0u, localLightCount, lights};
            sceneUniformBuffers[frame].update(&data, sizeof(data));
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
            const bool renderSceneViewport = editorUiActive && sceneViewportRendered;
            // ImGui owns a persistent descriptor for Scene View and may sample
            // it even before the viewport receives its first deferred redraw.
            // Use the normal render pass for a one-time clear: it also performs
            // the exact attachment-layout transition used by a real Scene View
            // render (including the MSAA resolve target).
            if (!renderSceneViewport && !sceneViewportImageInitialized) {
                forwardPass.begin(
                    commandBuffer, sceneViewportFramebuffer, sceneViewportTarget.extent(),
                    sceneDescriptorPass.descriptorSet(currentFrame), vertexBuffer.handle(),
                    instanceBuffers[currentFrame].handle(), indexBuffer.handle());
                ForwardPass::end(commandBuffer);
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
            const bool hasHiZResources = hiZBuffer.image() != VK_NULL_HANDLE;
            const bool hizEnabled = canUseHiZOcclusionCulling() && hasHiZResources;
            const bool hadPreviousHiZ = hiZValid;
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
            if (!sceneGpu.grassInstances.empty() && cameraController.camera()) {
                auto& lists = grassRenderLists[currentFrame];
                const auto camera = cameraController.camera();
                const GrassPackedCullUniformData cullData{camera->projectionMatrix().native() * camera->viewMatrix().native(), glm::vec4{camera->position().native(), 1.0F}, static_cast<uint32_t>(sceneGpu.grassInstances.size())};
                grassPackedCullUniformBuffers[currentFrame].update(&cullData, sizeof(cullData));
                const GrassClassifyUniformData classifyData{120.0F, 180.0F, 120.0F, 0.0F, glm::vec4{camera->position().native(), 1.0F}};
                grassClassifyUniformBuffers[currentFrame].update(&classifyData, sizeof(classifyData));
                const GrassPackedStreamUniformData streamData{static_cast<uint32_t>(sceneGpu.grassInstances.size()), 0U, static_cast<uint32_t>(sceneGpu.grassClusters.size()), 0U};
                for (uint32_t stream = 0; stream < 3; ++stream) { auto data = streamData; data.streamIndex = stream; grassPackedStreamUniformBuffers[currentFrame][stream].update(&data, sizeof(data)); }
                vkCmdFillBuffer(commandBuffer, lists.visibleCount.handle(), 0, VK_WHOLE_SIZE, 0);
                vkCmdFillBuffer(commandBuffer, lists.classifyCounts.handle(), 0, VK_WHOLE_SIZE, 0);
                for (uint32_t stream = 0; stream < 3; ++stream) {
                    vkCmdFillBuffer(commandBuffer, lists.binCounts[stream].handle(), 0, VK_WHOLE_SIZE, 0);
                    vkCmdFillBuffer(commandBuffer, lists.binCursors[stream].handle(), 0, VK_WHOLE_SIZE, 0);
                }
                vkCmdFillBuffer(commandBuffer, lists.mainDrawCount.handle(), 0, sizeof(uint32_t), 0);
                vkCmdFillBuffer(commandBuffer, lists.shadowDrawCount.handle(), 0, sizeof(uint32_t), 0);
                vkCmdFillBuffer(commandBuffer, lists.velocityDrawCount.handle(), 0, sizeof(uint32_t), 0);
                vkCmdFillBuffer(commandBuffer, lists.mainIndirect.handle(), 0, VK_WHOLE_SIZE, 0);
                vkCmdFillBuffer(commandBuffer, lists.shadowIndirect.handle(), 0, VK_WHOLE_SIZE, 0);
                vkCmdFillBuffer(commandBuffer, lists.velocityIndirect.handle(), 0, VK_WHOLE_SIZE, 0);
                const VkMemoryBarrier2 transferToCompute{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, nullptr, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT};
                const VkDependencyInfo dependency{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &transferToCompute}; vkCmdPipelineBarrier2(commandBuffer, &dependency);
                const auto dispatch = [&](VkPipeline pipeline, VkPipelineLayout layout, VkDescriptorSet set, uint32_t groups) { vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline); vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, nullptr); vkCmdDispatch(commandBuffer, groups, 1, 1); const VkMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, nullptr, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT}; const VkDependencyInfo info{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &barrier}; vkCmdPipelineBarrier2(commandBuffer, &info); };
                const uint32_t groups = (static_cast<uint32_t>(sceneGpu.grassInstances.size()) + 63U) / 64U;
                dispatch(grassPackedCullPipeline, grassPackedCullPipelineLayout, grassPackedCullSets[currentFrame], groups);
                dispatch(grassClassifyPipeline, grassClassifyPipelineLayout, grassClassifySets[currentFrame], groups);
                for (uint32_t stream = 0; stream < 3; ++stream) { dispatch(grassPackedBinPipeline, grassPackedBinPipelineLayout, grassPackedBinSets[currentFrame][stream], groups); dispatch(grassPackedPrefixPipeline, grassPrefixPipelineLayout, grassPackedPrefixSets[currentFrame][stream], 1); dispatch(grassPackedScatterPipeline, grassPackedScatterPipelineLayout, grassPackedScatterSets[currentFrame][stream], groups); dispatch(grassPackedFinalizePipeline, grassPackedFinalizePipelineLayout, grassPackedFinalizeSets[currentFrame][stream], (static_cast<uint32_t>(sceneGpu.grassClusters.size()) + 63U) / 64U); }
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
                const GrassPackedCullUniformData cullData{sceneCamera.projectionMatrix().native() * sceneCamera.viewMatrix().native(), glm::vec4{sceneCamera.position().native(), 1.0F}, static_cast<uint32_t>(sceneGpu.grassInstances.size())};
                sceneGrassPackedCullUniformBuffers[currentFrame].update(&cullData, sizeof(cullData));
                const GrassClassifyUniformData classifyData{120.0F, 180.0F, 120.0F, 0.0F, glm::vec4{sceneCamera.position().native(), 1.0F}};
                sceneGrassClassifyUniformBuffers[currentFrame].update(&classifyData, sizeof(classifyData));
                const GrassPackedStreamUniformData streamData{static_cast<uint32_t>(sceneGpu.grassInstances.size()), 0U, static_cast<uint32_t>(sceneGpu.grassClusters.size()), 0U};
                for (uint32_t stream = 0; stream < 3; ++stream) { auto data = streamData; data.streamIndex = stream; sceneGrassPackedStreamUniformBuffers[currentFrame][stream].update(&data, sizeof(data)); }
                vkCmdFillBuffer(commandBuffer, lists.visibleCount.handle(), 0, VK_WHOLE_SIZE, 0);
                vkCmdFillBuffer(commandBuffer, lists.classifyCounts.handle(), 0, VK_WHOLE_SIZE, 0);
                for (uint32_t stream = 0; stream < 3; ++stream) { vkCmdFillBuffer(commandBuffer, lists.binCounts[stream].handle(), 0, VK_WHOLE_SIZE, 0); vkCmdFillBuffer(commandBuffer, lists.binCursors[stream].handle(), 0, VK_WHOLE_SIZE, 0); }
                vkCmdFillBuffer(commandBuffer, lists.mainDrawCount.handle(), 0, sizeof(uint32_t), 0);
                vkCmdFillBuffer(commandBuffer, lists.mainIndirect.handle(), 0, VK_WHOLE_SIZE, 0);
                const VkMemoryBarrier2 transferToCompute{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, nullptr, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT};
                const VkDependencyInfo dependency{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &transferToCompute}; vkCmdPipelineBarrier2(commandBuffer, &dependency);
                const auto dispatch = [&](VkPipeline pipeline, VkPipelineLayout layout, VkDescriptorSet set, uint32_t groups) { vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline); vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, nullptr); vkCmdDispatch(commandBuffer, groups, 1, 1); const VkMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, nullptr, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT}; const VkDependencyInfo info{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &barrier}; vkCmdPipelineBarrier2(commandBuffer, &info); };
                const uint32_t groups = (static_cast<uint32_t>(sceneGpu.grassInstances.size()) + 63U) / 64U;
                dispatch(grassPackedCullPipeline, grassPackedCullPipelineLayout, sceneGrassPackedCullSets[currentFrame], groups);
                dispatch(grassClassifyPipeline, grassClassifyPipelineLayout, sceneGrassClassifySets[currentFrame], groups);
                // Scene View consumes only the main stream; its shadow and velocity
                // streams are intentionally left unused.
                constexpr uint32_t stream = 0;
                dispatch(grassPackedBinPipeline, grassPackedBinPipelineLayout, sceneGrassPackedBinSets[currentFrame][stream], groups);
                dispatch(grassPackedPrefixPipeline, grassPrefixPipelineLayout, sceneGrassPackedPrefixSets[currentFrame][stream], 1);
                dispatch(grassPackedScatterPipeline, grassPackedScatterPipelineLayout, sceneGrassPackedScatterSets[currentFrame][stream], groups);
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
                grassShadowDraw.create(lists.shadowIndirect.handle(), lists.shadowDrawCount.handle(),
                                       static_cast<uint32_t>(std::max<std::size_t>(1, sceneGpu.grassClusters.size())));
                grassShadowDrawPtr = &grassShadowDraw;
                shadowPass.setGrassShadowVisibleInstances(currentFrame, lists.drawInstances[1].handle());
                sceneDescriptorPass.setGrassShadowVisibleInstances(currentFrame, lists.drawInstances[1].handle());
            }

            // The forward descriptor layout always contains the shadow-map
            // sampler. Even when shadows are disabled, run an empty shadow
            // pass so its image is transitioned from UNDEFINED to
            // SHADER_READ_ONLY_OPTIMAL before the descriptor is used.
            shadowPass.record(
                commandBuffer, shadowClipMatrices, shadowClipUpdateMask, vertexBuffer.handle(),
                instanceBuffers[currentFrame].handle(), indexBuffer.handle(),
                shadowPass.descriptorSet(currentFrame), shadowCullingPasses[currentFrame],
                shadowIndirectDraws[currentFrame],
                optimizationFeatures.shadows && hasShadowCasters
                    ? static_cast<std::uint32_t>(gpuObjects.size()) : 0u,
                shadowPass.grassShadowDescriptorSet(currentFrame), grassShadowDrawPtr);

            // Scene View has its own descriptor pass and therefore its own
            // shadow-map image. It must be transitioned as well, even when
            // shadows are disabled, because the forward fragment shader still
            // samples the shadow binding declared by the shared pipeline.
            if (renderSceneViewport) {
                sceneDescriptorPass.record(
                    commandBuffer, sceneShadowClipMatrices, sceneShadowClipUpdateMask,
                    vertexBuffer.handle(), instanceBuffers[currentFrame].handle(), indexBuffer.handle(),
                    sceneDescriptorPass.descriptorSet(currentFrame), shadowCullingPasses[currentFrame],
                    shadowIndirectDraws[currentFrame],
                    optimizationFeatures.shadows && hasShadowCasters
                        ? static_cast<std::uint32_t>(gpuObjects.size()) : 0u,
                    sceneDescriptorPass.grassShadowDescriptorSet(currentFrame), grassShadowDrawPtr);
            }
            gpuCullingPasses[currentFrame].record(
                commandBuffer, static_cast<std::uint32_t>(gpuObjects.size()));
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
            foliageGpuCullingPasses[currentFrame].record(
                commandBuffer, static_cast<std::uint32_t>(gpuObjects.size()));

            // Never sample or resolve the multisampled depth attachment for
            // Hi-Z.  A separate 1x prepass gives the hierarchy a conventional
            // sampler2D source, then the main forward pass can use MSAA solely
            // for the visible rasterization.
            if (hizEnabled && msaa.enabled()) {
                hiZDepthPrepass.begin(
                    commandBuffer, hiZDepthPrepassFramebuffer, swapchain.extent(),
                    shadowPass.descriptorSet(currentFrame), vertexBuffer.handle(),
                    instanceBuffers[currentFrame].handle(), indexBuffer.handle());
                ForwardPass::draw(commandBuffer, indirectDraws[currentFrame]);
                hiZDepthPrepass.drawFoliage(commandBuffer, shadowPass.descriptorSet(currentFrame),
                                             foliageIndirectDraws[currentFrame]);
                ForwardPass::end(commandBuffer);

                VkImageMemoryBarrier2 depthReady{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
                depthReady.srcStageMask = VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
                depthReady.srcAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
                depthReady.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                depthReady.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
                // ForwardPass's render pass transitions the depth attachment
                // to READ_ONLY on exit, including its first use. Keep the
                // layout unchanged here and only add the visibility barrier.
                depthReady.oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
                depthReady.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
                depthReady.image = hiZDepthBuffer.image();
                depthReady.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
                VkDependencyInfo depthDependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
                depthDependency.imageMemoryBarrierCount = 1;
                depthDependency.pImageMemoryBarriers = &depthReady;
                vkCmdPipelineBarrier2(commandBuffer, &depthDependency);
                hiZPass.record(commandBuffer, hiZBuffer, true);
                hiZValid = true;
            }

            forwardPass.begin(
                commandBuffer, hdrFramebuffer, swapchain.extent(),
                shadowPass.descriptorSet(currentFrame), vertexBuffer.handle(),
                instanceBuffers[currentFrame].handle(), indexBuffer.handle());
            ForwardPass::draw(commandBuffer, indirectDraws[currentFrame]);
            forwardPass.drawFoliage(commandBuffer, shadowPass.descriptorSet(currentFrame),
                                    foliageIndirectDraws[currentFrame]);
            if (!sceneGpu.grassInstances.empty()) {
                const auto& lists = grassRenderLists[currentFrame];
                Culling::IndexedIndirectDrawCount grassDraw;
                grassDraw.create(lists.mainIndirect.handle(), lists.mainDrawCount.handle(),
                                 static_cast<uint32_t>(std::max<std::size_t>(1, sceneGpu.grassClusters.size())));
                shadowPass.setGrassVisibleInstances(currentFrame, lists.drawInstances[0].handle());
                forwardPass.drawGrass(commandBuffer, shadowPass.grassDescriptorSet(currentFrame), grassDraw);
            }
            // Fill the background before drawing particles. Otherwise the
            // skybox can overwrite transparent particle fragments in the sky.
            skyPass.record(commandBuffer, currentFrame);
            if (particleSystem && cameraController.camera()) {
                const Particles::ParticleFrameData particleFrame{
                    cameraController.camera()->projectionMatrix() * cameraController.camera()->viewMatrix(),
                    cameraController.camera()->right(),
                    0.0F,
                    cameraController.camera()->up(),
                    0.0F,
                };
                particleSystem->recordRender(commandBuffer, particleFrame,
                                             particlePipeline.handle(), particlePipeline.layout(),
                                              currentFrame, false);
            }
            forwardPass.drawOutline(commandBuffer, shadowPass.descriptorSet(currentFrame),
                                    indirectDraws[currentFrame]);
            ForwardPass::end(commandBuffer);

            if (taaResolveActive) {
                VkClearValue velocityClear{};
                VkRenderPassBeginInfo velocityPass{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
                velocityPass.renderPass = velocityPipeline.renderPass();
                velocityPass.framebuffer = velocityFramebuffer;
                velocityPass.renderArea.extent = swapchain.extent();
                velocityPass.clearValueCount = 1;
                velocityPass.pClearValues = &velocityClear;
                vkCmdBeginRenderPass(commandBuffer, &velocityPass, VK_SUBPASS_CONTENTS_INLINE);
                vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                  velocityPipeline.handle());
                const VkDescriptorSet sceneSet = shadowPass.descriptorSet(currentFrame);
                vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                        velocityPipeline.layout(), 0, 1, &sceneSet, 0, nullptr);
                const VkBuffer velocityVertexBuffers[] = {
                    vertexBuffer.handle(), instanceBuffers[currentFrame].handle()};
                constexpr VkDeviceSize velocityOffsets[] = {0, 0};
                vkCmdBindVertexBuffers(commandBuffer, 0, 2, velocityVertexBuffers,
                                       velocityOffsets);
                vkCmdBindIndexBuffer(commandBuffer, indexBuffer.handle(), 0, VK_INDEX_TYPE_UINT32);
                const VkViewport velocityViewport{0.0F, 0.0F,
                    static_cast<float>(swapchain.extent().width),
                    static_cast<float>(swapchain.extent().height), 0.0F, 1.0F};
                const VkRect2D velocityScissor{{0, 0}, swapchain.extent()};
                vkCmdSetViewport(commandBuffer, 0, 1, &velocityViewport);
                vkCmdSetScissor(commandBuffer, 0, 1, &velocityScissor);
                ForwardPass::draw(commandBuffer, indirectDraws[currentFrame]);
                vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                  foliageVelocityPipeline.handle());
                vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                        foliageVelocityPipeline.layout(), 0, 1, &sceneSet, 0, nullptr);
                ForwardPass::draw(commandBuffer, foliageIndirectDraws[currentFrame]);
                if (!sceneGpu.grassInstances.empty()) {
                    const auto& lists = grassRenderLists[currentFrame];
                    Culling::IndexedIndirectDrawCount grassVelocityDraw;
                    grassVelocityDraw.create(lists.velocityIndirect.handle(), lists.velocityDrawCount.handle(),
                                             static_cast<uint32_t>(std::max<std::size_t>(1, sceneGpu.grassClusters.size())));
                    shadowPass.setGrassVisibleInstances(currentFrame, lists.drawInstances[2].handle());
                    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                      grassVelocityPipeline.handle());
                    const VkDescriptorSet grassSet = shadowPass.grassDescriptorSet(currentFrame);
                    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                            grassVelocityPipeline.layout(), 0, 1, &grassSet, 0, nullptr);
                    grassVelocityDraw.record(commandBuffer);
                }
                vkCmdEndRenderPass(commandBuffer);
            }

            if (renderSceneViewport) {
                // Scene View has a separate frustum and therefore needs its own
                // indirect list. The game camera's list must not hide objects
                // which are visible from the editor camera.
                sceneGpuCullingPasses[currentFrame].record(
                    commandBuffer, static_cast<std::uint32_t>(gpuObjects.size()));
                sceneFoliageGpuCullingPasses[currentFrame].record(
                    commandBuffer, static_cast<std::uint32_t>(gpuObjects.size()));

                forwardPass.begin(
                    commandBuffer, sceneViewportFramebuffer, sceneViewportTarget.extent(),
                    sceneDescriptorPass.descriptorSet(currentFrame), vertexBuffer.handle(),
                    instanceBuffers[currentFrame].handle(), indexBuffer.handle());
                ForwardPass::draw(commandBuffer, sceneIndirectDraws[currentFrame]);
                forwardPass.drawFoliage(commandBuffer, sceneDescriptorPass.descriptorSet(currentFrame),
                                        sceneFoliageIndirectDraws[currentFrame]);
                if (!sceneGpu.grassInstances.empty()) {
                    const auto& lists = sceneGrassRenderLists[currentFrame];
                    Culling::IndexedIndirectDrawCount grassDraw;
                    grassDraw.create(lists.mainIndirect.handle(), lists.mainDrawCount.handle(),
                                     static_cast<uint32_t>(std::max<std::size_t>(1, sceneGpu.grassClusters.size())));
                    sceneDescriptorPass.setGrassVisibleInstances(currentFrame, lists.drawInstances[0].handle());
                    forwardPass.drawGrass(commandBuffer, sceneDescriptorPass.grassDescriptorSet(currentFrame), grassDraw);
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
                        sceneCamera.right(),
                        0.0F,
                        sceneCamera.up(),
                        0.0F,
                    };
                    particleSystem->recordRender(commandBuffer, particleFrame,
                                                 particlePipeline.handle(), particlePipeline.layout(),
                                                 currentFrame, true);
                }
                forwardPass.drawOutline(commandBuffer, sceneDescriptorPass.descriptorSet(currentFrame),
                                        sceneIndirectDraws[currentFrame]);
                ForwardPass::end(commandBuffer);
            }

            if (hizEnabled && !msaa.enabled()) {
                VkImageMemoryBarrier2 depthReady{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
                depthReady.srcStageMask = VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
                depthReady.srcAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
                depthReady.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                depthReady.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
                // The forward render pass already leaves depth in READ_ONLY.
                // This barrier supplies visibility for the following compute
                // pass without inventing an UNDEFINED transition.
                depthReady.oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
                depthReady.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
                depthReady.image = depthBuffer.image();
                depthReady.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
                VkDependencyInfo depthDependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
                depthDependency.imageMemoryBarrierCount = 1;
                depthDependency.pImageMemoryBarriers = &depthReady;
                vkCmdPipelineBarrier2(commandBuffer, &depthDependency);
                // The first-frame initialization above establishes a valid image
                // layout, so HiZPass must transition from SHADER_READ_ONLY rather
                // than from UNDEFINED when it builds the first hierarchy.
                hiZPass.record(commandBuffer, hiZBuffer, true);
                hiZValid = true;
            }

            if (taaResolveActive) {
                temporalAaPass.record(commandBuffer, swapchain.extent(), taaJitterX, taaJitterY);
            }

            if (editorUiActive) {
                VkRenderPassBeginInfo pass{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
                pass.renderPass = editorUiRenderPass;
                pass.framebuffer = editorUiFramebuffers.at(imageIndex.value);
                pass.renderArea.extent = swapchain.extent();
                constexpr float editorClearRed{0.06F};
                constexpr float editorClearGreen{0.07F};
                constexpr float editorClearBlue{0.09F};
                VkClearValue clear{};
                clear.color = {{editorClearRed, editorClearGreen, editorClearBlue, 1.0F}};
                pass.clearValueCount = 1;
                pass.pClearValues = &clear;
                vkCmdBeginRenderPass(commandBuffer, &pass, VK_SUBPASS_CONTENTS_INLINE);
                ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), commandBuffer);
                vkCmdEndRenderPass(commandBuffer);
            } else {
                if (taaResolveActive) {
                    tonemapPass.record(commandBuffer, imageIndex.value, swapchain.extent(), 0.0F,
                                       1U + temporalAaPass.resolvedIndex());
                } else {
                    tonemapPass.record(commandBuffer, imageIndex.value, swapchain.extent());
                }
                canvasRenderer.record(scene.uiCanvas(), commandBuffer, imageIndex.value, currentFrame,
                                      swapchain.extent());
            }

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
            destroyEditorUiResources();
            canvasRenderer.destroy();
            tonemapPass.destroy();
            temporalAaPass.destroy();
            destroyVelocityResources();
            if (hiZDepthPrepassFramebuffer != VK_NULL_HANDLE) {
                vkDestroyFramebuffer(device, hiZDepthPrepassFramebuffer, nullptr);
                hiZDepthPrepassFramebuffer = VK_NULL_HANDLE;
            }
            if (hdrFramebuffer != VK_NULL_HANDLE) {
                vkDestroyFramebuffer(device, hdrFramebuffer, nullptr);
                hdrFramebuffer = VK_NULL_HANDLE;
            }
            destroySceneViewportResources();

            msaa.destroy();
            hdrBuffer.destroy();
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

            if (hiZDepthPrepassFramebuffer != VK_NULL_HANDLE) {
                vkDestroyFramebuffer(device, hiZDepthPrepassFramebuffer, nullptr);
                hiZDepthPrepassFramebuffer = VK_NULL_HANDLE;
            }
            // Shadow/scene descriptor sets reference the packed grass
            // buffers owned by culling resources. They cannot outlive a
            // culling resize/rebuild.
            forwardPass.destroy();
            shadowPass.destroy();
            sceneDescriptorPass.destroy();
            destroyCullingResources();

            canvasRenderer.destroy();
            tonemapPass.destroy();
            temporalAaPass.destroy();
            destroyVelocityResources();
            if (hdrFramebuffer != VK_NULL_HANDLE) {
                vkDestroyFramebuffer(device, hdrFramebuffer, nullptr);
                hdrFramebuffer = VK_NULL_HANDLE;
            }
            destroySceneViewportResources();
            msaa.destroy();
            hdrBuffer.destroy();
            destroyDepthResources();
            destroyRenderFinishedSemaphores();

            swapchain.recreate();
            registry.view<CameraComponent>([&](const Entity, CameraComponent& component) {
                if (component.primary) {
                    component.setAspectRatio(static_cast<float>(swapchain.extent().width),
                                             static_cast<float>(swapchain.extent().height));
                }
            });
            hdrBuffer.create(vulkanDevice.physical(), device, swapchain.extent(), vulkanDevice.allocator());
            msaa.create(swapchain.extent(), HdrBuffer::Format);
            createDepthResources();
            createRenderFinishedSemaphores();

            createCullingResources();
            createShadowPass();
            createSceneDescriptorPass();
            createForwardPass();
            createFramebuffers();
            createSceneViewportResources();
            createTemporalAaPass();
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
                !cameraController.gameInputEnabled()) { return; }
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
                    if (registry.has<ColorPickerComponent>(particleEntity)) {
                        emitter.color = registry.get<ColorPickerComponent>(particleEntity).color;
                    }
                    particleSystem->setEmitter(emitter);
                } else {
                    auto emitter = registry.get<ParticleEmitterComponent>(particleEntity).emitter;
                    if (registry.has<Transform>(particleEntity)) {
                        emitter.position = registry.get<Transform>(particleEntity).position;
                    }
                    if (registry.has<ColorPickerComponent>(particleEntity)) {
                        emitter.color = registry.get<ColorPickerComponent>(particleEntity).color;
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
            VkSubmitInfo submitInfo{};
            submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;

            VkSemaphore waitSemaphores[] = {imageAvailableSemaphores[currentFrame]};
            VkPipelineStageFlags waitStages[] = {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
            submitInfo.waitSemaphoreCount = 1;
            submitInfo.pWaitSemaphores = waitSemaphores;
            submitInfo.pWaitDstStageMask = waitStages;
            submitInfo.commandBufferCount = 1;
            submitInfo.pCommandBuffers = &commandBuffers[currentFrame];

            VkSemaphore signalSemaphores[] = {renderFinishedSemaphores[imageIndex]};
            submitInfo.signalSemaphoreCount = 1;
            submitInfo.pSignalSemaphores = signalSemaphores;

            const VkResult submitResult = vkQueueSubmit(
                vulkanDevice.graphicsQueue(), 1, &submitInfo, inFlightFences[currentFrame]);
            if (submitResult != VK_SUCCESS) {
                throw std::runtime_error(
                    "Could not submit command buffer to queue (VkResult " +
                    std::to_string(static_cast<int>(submitResult)) + ")");
            }
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
            updateRenderableBuffers();
            const Vec3 sceneCameraPosition = cameraController.editorPosition();
            const float sceneCameraYaw = cameraController.editorYaw();
            const float sceneCameraPitch = cameraController.editorPitch();
            const bool sceneCameraChanged = !sceneViewportCacheValid ||
                sceneCameraPosition.x() != renderedSceneViewportPosition.x() ||
                sceneCameraPosition.y() != renderedSceneViewportPosition.y() ||
                sceneCameraPosition.z() != renderedSceneViewportPosition.z() ||
                sceneCameraYaw != renderedSceneViewportYaw ||
                sceneCameraPitch != renderedSceneViewportPitch;
            // The direct single-sample target is also exposed to ImGui. Keep
            // its established every-frame render path until it has a dedicated
            // cache-compatible render pass; MSAA uses a separate resolve image
            // and can safely retain the deferred Scene View cache.
            sceneViewportRendered = sceneViewportActive &&
                (!msaa.enabled() || sceneViewportNeedsRender || sceneCameraChanged ||
                 scene.mutationRevision() != sceneViewportRenderedRevision);
            // Scene View is deliberately not rendered in play mode. Its cache
            // cannot consume this frame's dirty list, so discard it lazily;
            // the active game-view cache is updated page-by-page below.
            if (!sceneViewportActive && !dirtyShadowObjects.empty()) {
                sceneDescriptorPass.invalidateCache();
            }
            updateUniformBuffer(currentFrame);
            if (sceneViewportRendered) {
                updateSceneViewportUniformBuffer(currentFrame);
            }
            updateParticleSystemForFrame();
            updateCullingUniformBuffer(currentFrame);
            if (sceneViewportRendered) {
                updateSceneCullingUniformBuffer(currentFrame);
            }
            if (optimizationFeatures.shadows) {
                updateShadowCullingUniformBuffer(currentFrame);
            }
            recordCommandBuffer(commandBuffers[currentFrame], SwapchainImageIndex{imageIndex});
            submitAndPresentFrame(imageIndex);
            if (sceneViewportRendered) {
                renderedSceneViewportPosition = sceneCameraPosition;
                renderedSceneViewportYaw = sceneCameraYaw;
                renderedSceneViewportPitch = sceneCameraPitch;
                sceneViewportRenderedRevision = scene.mutationRevision();
                sceneViewportNeedsRender = false;
                sceneViewportCacheValid = true;
                sceneViewportImageInitialized = true;
            }

            currentFrame = (currentFrame + 1) % MAX_FRAMES_IN_FLIGHT;
            ++shadowClipFrameIndex;
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
