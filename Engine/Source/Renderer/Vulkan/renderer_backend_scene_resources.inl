        void createMaterialTextures() {
            constexpr std::array<std::uint8_t, 4> white = {255, 255, 255, 255};
            fallbackMaterialTexture.create(
                vulkanDevice.physical(), device, commandPool, vulkanDevice.graphicsQueue(),
                1, 1, white, TextureColorSpace::SRGB, false, vulkanDevice.allocator());
            const VkDescriptorImageInfo fallback{
                fallbackMaterialTexture.sampler(), fallbackMaterialTexture.imageView(),
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            materialTextureDescriptors.assign(MaxMaterialTextures, fallback);

            std::unordered_set<const Mesh*> uploaded;
            registry.view<MeshRenderer>([&](const Entity, const MeshRenderer& renderer) {
                if (!renderer.hasMesh() || !uploaded.insert(renderer.mesh.get()).second) return;
                const Mesh& mesh = *renderer.mesh;
                const auto offset = static_cast<std::uint32_t>(materialTextures.size() + 1);
                if (mesh.images.size() > MaxMaterialTextures - offset) {
                    throw std::runtime_error("GLB scene exceeds the material texture limit");
                }
                meshTextureOffsets.emplace(&mesh, offset);
                for (std::size_t i = 0; i < mesh.images.size(); ++i) {
                    const Mesh::Image& image = mesh.images[i];
                    const bool isBaseColorTexture = std::ranges::any_of(
                        mesh.materials, [i](const PBRMaterial& material) {
                            return material.baseColorTexture == static_cast<std::int32_t>(i);
                        });
                    if (image.width == 0 || image.height == 0 || image.rgbaPixels.empty()) {
                        materialTextures.emplace_back();
                        continue;
                    }
                    Texture2D texture;
                    texture.create(vulkanDevice.physical(), device, commandPool,
                                   vulkanDevice.graphicsQueue(), image.width, image.height,
                                   image.rgbaPixels, isBaseColorTexture ? TextureColorSpace::SRGB
                                                                        : TextureColorSpace::Linear,
                                   true,
                                   vulkanDevice.allocator());
                    materialTextureDescriptors[offset + i] = {
                        texture.sampler(), texture.imageView(),
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
                    materialTextures.push_back(std::move(texture));
                }
            });
            registry.view<TerrainGrassComponent>([&](const Entity, const TerrainGrassComponent& grass) {
                if (!grass.hasPrefab() || !uploaded.insert(grass.mesh.get()).second) return;
                const Mesh& mesh = *grass.mesh;
                const auto offset = static_cast<std::uint32_t>(materialTextures.size() + 1);
                if (mesh.images.size() > MaxMaterialTextures - offset) {
                    throw std::runtime_error("Grass prefab exceeds the material texture limit");
                }
                meshTextureOffsets.emplace(&mesh, offset);
                for (std::size_t i = 0; i < mesh.images.size(); ++i) {
                    const Mesh::Image& image = mesh.images[i];
                    const bool srgb = std::ranges::any_of(mesh.materials, [i](const PBRMaterial& material) {
                        return material.baseColorTexture == static_cast<std::int32_t>(i);
                    });
                    if (image.width == 0 || image.height == 0 || image.rgbaPixels.empty()) {
                        materialTextures.emplace_back();
                        continue;
                    }
                    Texture2D texture;
                    texture.create(vulkanDevice.physical(), device, commandPool,
                                   vulkanDevice.graphicsQueue(), image.width, image.height,
                                   image.rgbaPixels, srgb ? TextureColorSpace::SRGB
                                                          : TextureColorSpace::Linear,
                                   true, vulkanDevice.allocator());
                    materialTextureDescriptors[offset + i] = {
                        texture.sampler(), texture.imageView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
                    materialTextures.push_back(std::move(texture));
                }
            });
        }

        [[nodiscard]] std::uint64_t currentRenderableTopologySignature() const {
            // Commutative mixing makes the value independent of dense-pool
            // ordering, which can change after an unrelated ECS removal.
            constexpr std::uint64_t topologySignatureSeed = 14695981039346656037ULL;
            constexpr std::uint64_t hashCombineConstant = 0x9e3779b97f4a7c15ULL;
            constexpr std::uint32_t hashCombineLeftShift = 6U;
            std::uint64_t signature = topologySignatureSeed;
            std::size_t count = 0;
            const Registry& readRegistry = registry;
            readRegistry.view<Transform, MeshRenderer>(
                [&](const Entity entity, const Transform&, const MeshRenderer& renderer) {
                    if (!renderer.hasMesh()) return;
                    std::uint64_t value = static_cast<std::uint64_t>(entity);
                    value ^= static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(renderer.mesh.get())) +
                        hashCombineConstant + (value << hashCombineLeftShift) + (value >> 2u);
                    value ^= static_cast<std::uint64_t>(renderer.cullingBatch) << 1u;
                    value ^= static_cast<std::uint64_t>(renderer.castShadow) << 63u;
                    signature ^= value * hashCombineConstant;
                    ++count;
                });
            readRegistry.view<Transform, TerrainGrassComponent>(
                [&](const Entity entity, const Transform&, const TerrainGrassComponent& grass) {
                    if (!grass.hasPrefab() || grass.instances.empty()) return;
                    std::uint64_t value = static_cast<std::uint64_t>(entity);
                    value ^= static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(grass.mesh.get())) +
                        hashCombineConstant + (value << hashCombineLeftShift) + (value >> 2u);
                    value ^= static_cast<std::uint64_t>(grass.instances.size()) << 17u;
                    value ^= readRegistry.componentRevision<TerrainGrassComponent>();
                    signature ^= value * hashCombineConstant;
                    count += grass.instances.size();
                });

            // Particle resources are derived from the emitter entity, but do
            // not participate in the regular renderable list above. Include
            // them in the signature so removing an emitter cannot leave the
            // old ParticleSystem alive on the GPU.
            const Entity particleEntity = scene.particleEntity();
            std::uint64_t particleValue = static_cast<std::uint64_t>(particleEntity);
            particleValue ^= static_cast<std::uint64_t>(
                readRegistry.has<SmokeEmitterComponent>(particleEntity)) << 1u;
            particleValue ^= static_cast<std::uint64_t>(
                readRegistry.has<ParticleEmitterComponent>(particleEntity)) << 2u;
            signature ^= particleValue * hashCombineConstant;
            return signature ^ (static_cast<std::uint64_t>(count) * hashCombineConstant);
        }

        void createMeshBuffers() {
            Mesh sceneMesh;
            renderables.reserve(registry.size());
            renderables.clear();
            instanceBatches.clear();
            sceneGpu.batchRenderableIndices.clear();
            sceneGpu.renderableIndices.clear();
            sceneGpu.grassRenderableIndices.clear();
            instanceBatches.reserve(registry.size());
            sceneGpu.batchRenderableIndices.reserve(registry.size());
            glm::vec3 sceneMinimum{std::numeric_limits<float>::max()};
            glm::vec3 sceneMaximum{std::numeric_limits<float>::lowest()};
            // Each MeshRenderer retains its own draw range, but identical
            // meshes contribute their geometry to the GPU buffers only once.
            struct MeshUploadRecord {
                uint32_t firstIndex;
                uint32_t firstVertex;
                AABB localBounds;
            };
            struct BatchKey {
                const Mesh* mesh;
                bool castShadow;
                uint32_t cullingBatch;

                bool operator==(const BatchKey& other) const noexcept {
                    return mesh == other.mesh && castShadow == other.castShadow &&
                           cullingBatch == other.cullingBatch;
                }
            };
            struct BatchKeyHash {
                std::size_t operator()(const BatchKey& key) const noexcept {
                    constexpr std::uint32_t hashCombineConstant = 0x9e3779b9U;
                    constexpr std::uint32_t hashCombineLeftShift = 6U;
                    const auto meshHash = std::hash<const Mesh*>{}(key.mesh);
                    const auto batchHash = std::hash<uint32_t>{}(key.cullingBatch);
                    return meshHash ^ (batchHash + static_cast<std::size_t>(key.castShadow) +
                                       hashCombineConstant + (meshHash << hashCombineLeftShift) +
                                       (meshHash >> 2U));
                }
            };
            std::unordered_map<const Mesh*, MeshUploadRecord> uploadedMeshes;
            uploadedMeshes.reserve(registry.size());
            std::unordered_map<BatchKey, std::size_t, BatchKeyHash> batchIndices;
            batchIndices.reserve(registry.size());
            std::unordered_set<const Mesh*> uniqueMeshes;
            uniqueMeshes.reserve(registry.size());
            std::size_t vertexCapacity = 0;
            std::size_t indexCapacity = 0;
            materialSlots = 1;
            registry.view<MeshRenderer>([&](const Entity, const MeshRenderer& renderer) {
                if (!renderer.hasMesh() || !uniqueMeshes.insert(renderer.mesh.get()).second) {
                    return;
                }
                vertexCapacity += renderer.mesh->vertices.size();
                indexCapacity += renderer.mesh->indices.size();
                materialSlots = std::max(materialSlots, static_cast<std::uint32_t>(
                    std::max<std::size_t>(1, renderer.mesh->materials.size())));
            });
            registry.view<TerrainGrassComponent>([&](const Entity, const TerrainGrassComponent& grass) {
                if (!grass.hasPrefab() || !uniqueMeshes.insert(grass.mesh.get()).second) return;
                vertexCapacity += grass.mesh->vertices.size();
                indexCapacity += grass.mesh->indices.size();
                materialSlots = std::max(materialSlots, static_cast<std::uint32_t>(
                    std::max<std::size_t>(1, grass.mesh->materials.size())));
            });
            sceneMesh.vertices.reserve(vertexCapacity);
            sceneMesh.indices.reserve(indexCapacity);
            registry.view<Transform, MeshRenderer>(
                [&](const Entity entity, const Transform& transform, MeshRenderer& renderer) {
                    if (!renderer.hasMesh()) {
                        return;
                    }

                    const Mesh* const mesh = renderer.mesh.get();
                    AABB localBounds;
                    std::uint32_t firstVertex = 0;
                    if (optimizationFeatures.meshDeduplication) {
                        const auto existing = uploadedMeshes.find(mesh);
                        if (existing != uploadedMeshes.end()) {
                        renderer.firstIndex = existing->second.firstIndex;
                        firstVertex = existing->second.firstVertex;
                        localBounds = existing->second.localBounds;
                        } else {
                            if (sceneMesh.vertices.size() + mesh->vertices.size() >
                                    std::numeric_limits<uint32_t>::max() ||
                                sceneMesh.indices.size() + mesh->indices.size() >
                                    std::numeric_limits<uint32_t>::max()) {
                                throw std::runtime_error("Scene geometry exceeds 32-bit draw limits");
                            }
                            const uint32_t vertexOffset = sceneMesh.vertexCount();
                            firstVertex = vertexOffset;
                            renderer.firstIndex = sceneMesh.indexCount();
                            sceneMesh.vertices.insert(sceneMesh.vertices.end(),
                                                      mesh->vertices.begin(), mesh->vertices.end());
                            for (const uint32_t index : mesh->indices) {
                                sceneMesh.indices.push_back(vertexOffset + index);
                            }
                            localBounds = {
                            .min = Vec3{std::numeric_limits<float>::max(),
                                        std::numeric_limits<float>::max(),
                                        std::numeric_limits<float>::max()},
                            .max = Vec3{std::numeric_limits<float>::lowest(),
                                        std::numeric_limits<float>::lowest(),
                                        std::numeric_limits<float>::lowest()},
                            };
                            for (const Vertex& vertex : mesh->vertices) {
                                localBounds.min.setX(std::min(localBounds.min.x(), vertex.position.x()));
                                localBounds.min.setY(std::min(localBounds.min.y(), vertex.position.y()));
                                localBounds.min.setZ(std::min(localBounds.min.z(), vertex.position.z()));
                                localBounds.max.setX(std::max(localBounds.max.x(), vertex.position.x()));
                                localBounds.max.setY(std::max(localBounds.max.y(), vertex.position.y()));
                                localBounds.max.setZ(std::max(localBounds.max.z(), vertex.position.z()));
                            }
                            uploadedMeshes.emplace(mesh, MeshUploadRecord{
                                renderer.firstIndex, firstVertex, localBounds});
                        }
                    } else {
                        if (sceneMesh.vertices.size() + mesh->vertices.size() >
                                std::numeric_limits<uint32_t>::max() ||
                            sceneMesh.indices.size() + mesh->indices.size() >
                                std::numeric_limits<uint32_t>::max()) {
                            throw std::runtime_error("Scene geometry exceeds 32-bit draw limits");
                        }
                        const uint32_t vertexOffset = sceneMesh.vertexCount();
                        firstVertex = vertexOffset;
                        renderer.firstIndex = sceneMesh.indexCount();
                        sceneMesh.vertices.insert(sceneMesh.vertices.end(),
                                                  mesh->vertices.begin(), mesh->vertices.end());
                        for (const uint32_t index : mesh->indices) {
                            sceneMesh.indices.push_back(vertexOffset + index);
                        }
                        localBounds = {
                        .min = Vec3{std::numeric_limits<float>::max(),
                                    std::numeric_limits<float>::max(),
                                    std::numeric_limits<float>::max()},
                        .max = Vec3{std::numeric_limits<float>::lowest(),
                                    std::numeric_limits<float>::lowest(),
                                    std::numeric_limits<float>::lowest()},
                        };
                        for (const Vertex& vertex : mesh->vertices) {
                        localBounds.min.setX(std::min(localBounds.min.x(), vertex.position.x()));
                        localBounds.min.setY(std::min(localBounds.min.y(), vertex.position.y()));
                        localBounds.min.setZ(std::min(localBounds.min.z(), vertex.position.z()));
                        localBounds.max.setX(std::max(localBounds.max.x(), vertex.position.x()));
                        localBounds.max.setY(std::max(localBounds.max.y(), vertex.position.y()));
                        localBounds.max.setZ(std::max(localBounds.max.z(), vertex.position.z()));
                        }
                    }
                    const AABB worldBounds =
                        localBounds.transformed(transform.matrix().native());
                    const bool castShadow = renderer.castShadow;
                    const BatchKey batchKey{mesh, castShadow, renderer.cullingBatch};
                    const auto [batchIt, inserted] = optimizationFeatures.instancedRendering
                        ? batchIndices.try_emplace(batchKey, instanceBatches.size())
                        : std::pair{batchIndices.end(), true};
                    const std::size_t batchIndex = optimizationFeatures.instancedRendering
                        ? batchIt->second : instanceBatches.size();
                    if (inserted) {
                        instanceBatches.push_back(InstanceBatch{
                            .mesh = mesh,
                            .firstIndex = renderer.firstIndex,
                            .indexCount = mesh->indexCount(),
                            .firstInstance = static_cast<uint32_t>(renderables.size()),
                            .instanceCount = 0,
                            .castShadow = castShadow,
                            .worldBounds = worldBounds,
                        });
                        sceneGpu.batchRenderableIndices.emplace_back();
                    }
                    InstanceBatch& batch = instanceBatches[batchIndex];
                    if (batch.instanceCount == 0) {
                        batch.worldBounds = worldBounds;
                    } else {
                        batch.worldBounds.min = Vec3{
                            std::min(batch.worldBounds.min.x(), worldBounds.min.x()),
                            std::min(batch.worldBounds.min.y(), worldBounds.min.y()),
                            std::min(batch.worldBounds.min.z(), worldBounds.min.z())};
                        batch.worldBounds.max = Vec3{
                            std::max(batch.worldBounds.max.x(), worldBounds.max.x()),
                            std::max(batch.worldBounds.max.y(), worldBounds.max.y()),
                            std::max(batch.worldBounds.max.z(), worldBounds.max.z())};
                    }
                    ++batch.instanceCount;
                    renderables.push_back({entity, localBounds, batchIndex,
                                           firstVertex, mesh->vertexCount()});
                    const std::size_t renderableIndex = renderables.size() - 1;
                    sceneGpu.batchRenderableIndices[batchIndex].push_back(renderableIndex);
                    sceneGpu.renderableIndices[entity] = renderableIndex;
                    sceneMinimum = glm::min(sceneMinimum, worldBounds.min.native());
                    sceneMaximum = glm::max(sceneMaximum, worldBounds.max.native());
                });

            // Painted grass stays compact in the ECS and is expanded straight
            // into a contiguous GPU instance batch per terrain.
            registry.view<Transform, TerrainGrassComponent>(
                [&](const Entity entity, const Transform& terrainTransform,
                    const TerrainGrassComponent& grass) {
                    if (!grass.hasPrefab() || grass.instances.empty()) return;
                    const Mesh* mesh = grass.mesh.get();
                    MeshUploadRecord upload{};
                    if (const auto found = uploadedMeshes.find(mesh); found != uploadedMeshes.end()) {
                        upload = found->second;
                    } else {
                        if (sceneMesh.vertices.size() + mesh->vertices.size() >
                                std::numeric_limits<uint32_t>::max() ||
                            sceneMesh.indices.size() + mesh->indices.size() >
                                std::numeric_limits<uint32_t>::max()) {
                            throw std::runtime_error("Scene geometry exceeds 32-bit draw limits");
                        }
                        upload.firstVertex = sceneMesh.vertexCount();
                        upload.firstIndex = sceneMesh.indexCount();
                        upload.localBounds = {
                            .min = Vec3{std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
                                        std::numeric_limits<float>::max()},
                            .max = Vec3{std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest(),
                                        std::numeric_limits<float>::lowest()},
                        };
                        sceneMesh.vertices.insert(sceneMesh.vertices.end(), mesh->vertices.begin(), mesh->vertices.end());
                        for (const std::uint32_t index : mesh->indices)
                            sceneMesh.indices.push_back(upload.firstVertex + index);
                        for (const Vertex& vertex : mesh->vertices) {
                            upload.localBounds.min = Vec3{glm::min(upload.localBounds.min.native(), vertex.position.native())};
                            upload.localBounds.max = Vec3{glm::max(upload.localBounds.max.native(), vertex.position.native())};
                        }
                        uploadedMeshes.emplace(mesh, upload);
                    }

                    const std::size_t batchIndex = instanceBatches.size();
                    const std::uint32_t firstInstance = static_cast<std::uint32_t>(renderables.size());
                    std::vector<std::size_t> grassIndices;
                    grassIndices.reserve(grass.instances.size());
                    AABB batchBounds{};
                    bool firstBounds = true;
                    for (std::size_t i = 0; i < grass.instances.size(); ++i) {
                        const auto& item = grass.instances[i];
                        const Transform local{.position = item.position,
                                              .rotation = Vec3{0.0F, item.yaw, 0.0F},
                                              .scale = Vec3{item.scale, item.scale, item.scale}};
                        const glm::mat4 model = terrainTransform.matrix().native() * local.matrix().native();
                        const AABB world = upload.localBounds.transformed(model);
                        if (firstBounds) { batchBounds = world; firstBounds = false; }
                        else {
                            batchBounds.min = Vec3{glm::min(batchBounds.min.native(), world.min.native())};
                            batchBounds.max = Vec3{glm::max(batchBounds.max.native(), world.max.native())};
                        }
                        renderables.push_back({entity, upload.localBounds, batchIndex,
                                               upload.firstVertex, mesh->vertexCount(), i});
                        const std::size_t renderableIndex = renderables.size() - 1;
                        grassIndices.push_back(renderableIndex);
                    }
                    const auto largestInstance = std::ranges::max_element(
                        grass.instances, {}, &TerrainGrassInstance::scale);
                    const float horizontalTerrainScale = std::max(
                        std::abs(terrainTransform.scale.x()), std::abs(terrainTransform.scale.z()));
                    const float bendExpansion = (upload.localBounds.max.y() - upload.localBounds.min.y()) *
                        largestInstance->scale * horizontalTerrainScale * 0.8F;
                    batchBounds.min.setX(batchBounds.min.x() - bendExpansion);
                    batchBounds.min.setZ(batchBounds.min.z() - bendExpansion);
                    batchBounds.max.setX(batchBounds.max.x() + bendExpansion);
                    batchBounds.max.setZ(batchBounds.max.z() + bendExpansion);
                    sceneGpu.grassRenderableIndices[entity] = grassIndices;
                    sceneGpu.batchRenderableIndices.push_back(std::move(grassIndices));
                    instanceBatches.push_back(InstanceBatch{.mesh = mesh,
                        .firstIndex = upload.firstIndex, .indexCount = mesh->indexCount(),
                        .firstInstance = firstInstance,
                        .instanceCount = static_cast<std::uint32_t>(grass.instances.size()),
                        .castShadow = grass.castShadow, .worldBounds = batchBounds});
                    sceneMinimum = glm::min(sceneMinimum, batchBounds.min.native());
                    sceneMaximum = glm::max(sceneMaximum, batchBounds.max.native());
                });

            // An editor scene is allowed to be empty.  Render passes still
            // bind vertex/index/instance/material buffers even when there are
            // no draw calls, so keep one harmless dummy element in each GPU
            // buffer instead of failing scene synchronization after deleting
            // the final mesh object.
            if (sceneMesh.empty()) {
                constexpr Vertex dummyVertex{};
                constexpr std::uint32_t dummyIndex = 0;
                hasShadowCasters = false;
                sceneCenter = Vec3{};
                sceneRadius = 1.0F;
                vertexBuffer.createDeviceLocal(
                    vulkanDevice.physical(), device, &dummyVertex, sizeof(dummyVertex),
                    VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, commandPool,
                    vulkanDevice.graphicsQueue(), vulkanDevice.allocator());
                indexBuffer.createDeviceLocal(
                    vulkanDevice.physical(), device, &dummyIndex, sizeof(dummyIndex),
                    VK_BUFFER_USAGE_INDEX_BUFFER_BIT, commandPool,
                    vulkanDevice.graphicsQueue(), vulkanDevice.allocator());
                return;
            }

            hasShadowCasters = false;
            for (const InstanceBatch& batch : instanceBatches) {
                if (batch.castShadow) {
                    hasShadowCasters = true;
                    break;
                }
            }

            const glm::vec3 center = (sceneMinimum + sceneMaximum) * 0.5F;
            const glm::vec3 halfExtent = (sceneMaximum - sceneMinimum) * 0.5F;
            sceneCenter = Vec3{center};
            sceneRadius = std::max({halfExtent.x, halfExtent.y, halfExtent.z, 1.0F});

            vertexBuffer.createDeviceLocal(vulkanDevice.physical(), device, sceneMesh.vertices.data(),
                sizeof(Vertex) * sceneMesh.vertices.size(), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                commandPool, vulkanDevice.graphicsQueue(), vulkanDevice.allocator());
            indexBuffer.createDeviceLocal(vulkanDevice.physical(), device, sceneMesh.indices.data(),
                sizeof(uint32_t) * sceneMesh.indices.size(), VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                commandPool, vulkanDevice.graphicsQueue(), vulkanDevice.allocator());
        }

        void createInstanceBuffer() {
            instanceModels.resize(renderables.size());
            materials.resize(renderables.size() * materialSlots);
            updateRenderableBuffers();
            for (Buffer& buffer : instanceBuffers) {
                buffer.createHostVisible(vulkanDevice.physical(), device,
                    sizeof(RendererInstanceData) * std::max<std::size_t>(1, instanceModels.size()),
                    VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                    vulkanDevice.allocator());
                if (!instanceModels.empty()) {
                    buffer.update(instanceModels.data(),
                                  sizeof(RendererInstanceData) * instanceModels.size());
                }
            }
            for (Buffer& buffer : materialBuffers) {
                buffer.createHostVisible(vulkanDevice.physical(), device,
                    sizeof(GPUMaterialData) * std::max<std::size_t>(1, materials.size()),
                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, vulkanDevice.allocator());
                if (!materials.empty()) {
                    buffer.update(materials.data(), sizeof(GPUMaterialData) * materials.size());
                }
            }

            // Every instance/material buffer has just received the complete
            // CPU snapshot, so no incremental upload is pending for it.
            for (RenderableRecord& record : renderables) {
                record.transformDirtyFrames = 0;
                record.materialDirtyFrames = 0;
            }
            for (auto& indices : dirtyTransforms) indices.clear();
            for (auto& indices : dirtyMaterials) indices.clear();
        }

        template <typename T>
        struct DirtyRangeUploadRequest {
            const Buffer& buffer;
            const std::vector<T>& data;
            uint8_t RenderableRecord::* dirtyFrames;
            uint8_t bit;
        };

        template <typename T>
        void uploadDirtyRanges(const DirtyRangeUploadRequest<T>& request) const {
            std::size_t rangeBegin = 0;
            while (rangeBegin < renderables.size()) {
                while (rangeBegin < renderables.size() &&
                       (renderables[rangeBegin].*request.dirtyFrames & request.bit) == 0) {
                    ++rangeBegin;
                }
                std::size_t rangeEnd = rangeBegin;
                while (rangeEnd < renderables.size() &&
                       (renderables[rangeEnd].*request.dirtyFrames & request.bit) != 0) {
                    ++rangeEnd;
                }
                if (rangeBegin != rangeEnd) {
                    request.buffer.update(request.data.data() + rangeBegin,
                                  sizeof(T) * (rangeEnd - rangeBegin),
                                  sizeof(T) * rangeBegin);
                }
                rangeBegin = rangeEnd;
            }
        }

        template <typename T>
        struct DirtyIndexUploadRequest {
            const Buffer& buffer;
            const std::vector<T>& data;
            uint8_t RenderableRecord::* dirtyFrames;
            const std::vector<std::size_t>& indices;
        };

        template <typename T>
        static void uploadDirtyIndices(const DirtyIndexUploadRequest<T>& request) {
            std::size_t rangeStart = 0;
            while (rangeStart < request.indices.size()) {
                std::size_t rangeEnd = rangeStart + 1;
                while (rangeEnd < request.indices.size() &&
                       request.indices[rangeEnd] == request.indices[rangeEnd - 1] + 1) {
                    ++rangeEnd;
                }
                const std::size_t first = request.indices[rangeStart];
                request.buffer.update(request.data.data() + first,
                              sizeof(T) * (rangeEnd - rangeStart),
                              sizeof(T) * first);
                rangeStart = rangeEnd;
            }
        }

        void clearDirtyIndices(uint8_t RenderableRecord::* dirtyFrames,
                               std::vector<std::size_t>& indices, const uint8_t bit) const {
            for (const std::size_t index : indices) {
                renderables[index].*dirtyFrames &= static_cast<uint8_t>(~bit);
            }
            indices.clear();
        }

        // Each frame in flight owns a separate GPU buffer.  A scene mutation
        // must therefore be uploaded once per buffer, even after the registry
        // revision itself has stopped changing.
        void uploadPendingRenderableBuffers() {
            if (instanceBuffers[currentFrame].handle() == VK_NULL_HANDLE) { return;
}

            const uint8_t bit = frameBit(currentFrame);
            uploadDirtyIndices(DirtyIndexUploadRequest<RendererInstanceData>{
                instanceBuffers[currentFrame], instanceModels,
                &RenderableRecord::transformDirtyFrames, dirtyTransforms[currentFrame]});
            clearDirtyIndices(&RenderableRecord::transformDirtyFrames,
                              dirtyTransforms[currentFrame], bit);
            for (const std::size_t index : dirtyMaterials[currentFrame]) {
                materialBuffers[currentFrame].update(
                    materials.data() + index * materialSlots,
                    sizeof(GPUMaterialData) * materialSlots,
                    sizeof(GPUMaterialData) * index * materialSlots);
            }
            clearDirtyIndices(&RenderableRecord::materialDirtyFrames,
                              dirtyMaterials[currentFrame], bit);
        }

        void updateRenderableBuffers() {
            const std::uint64_t transformRevision = registry.componentRevision<Transform>();
            const std::uint64_t meshRendererRevision = registry.componentRevision<MeshRenderer>();
            const std::uint64_t terrainGrassRevision = registry.componentRevision<TerrainGrassComponent>();
            if (transformRevision == lastTransformRevision &&
                meshRendererRevision == lastMeshRendererRevision &&
                terrainGrassRevision == lastTerrainGrassRevision) {
                uploadPendingRenderableBuffers();
                return;
            }

            std::vector<std::size_t> changedIndices;
            changedIndices.reserve(renderables.size());
            if (lastTransformRevision == std::numeric_limits<std::uint64_t>::max() ||
                lastMeshRendererRevision == std::numeric_limits<std::uint64_t>::max()) {
                for (std::size_t index = 0; index < renderables.size(); ++index) {
                    changedIndices.push_back(index);
                }
            } else {
                std::unordered_set<std::size_t> uniqueIndices;
                const auto addChangedEntities = [&](const auto& entities, const auto revision) {
                    if (revision == 0) { return;
}
                    for (const Entity entity : entities) {
                        const auto it = sceneGpu.renderableIndices.find(entity);
                        if (it != sceneGpu.renderableIndices.end()) uniqueIndices.insert(it->second);
                        const auto grass = sceneGpu.grassRenderableIndices.find(entity);
                        if (grass != sceneGpu.grassRenderableIndices.end()) {
                            uniqueIndices.insert(grass->second.begin(), grass->second.end());
                        }
                    }
                };
                addChangedEntities(
                    registry.componentEntitiesChangedSince<Transform>(lastTransformRevision),
                    transformRevision);
                addChangedEntities(
                    registry.componentEntitiesChangedSince<MeshRenderer>(lastMeshRendererRevision),
                    meshRendererRevision);
                for (const Entity entity : registry.componentEntitiesChangedSince<TerrainGrassComponent>(
                         lastTerrainGrassRevision)) {
                    const auto mapped = sceneGpu.grassRenderableIndices.find(entity);
                    if (mapped == sceneGpu.grassRenderableIndices.end() ||
                        !registry.has<TerrainGrassComponent>(entity)) continue;
                    const auto& grass = registry.get<TerrainGrassComponent>(entity);
                    if (grass.allInstancesDirty || grass.dirtyInstances.empty()) {
                        uniqueIndices.insert(mapped->second.begin(), mapped->second.end());
                    } else {
                        for (const std::size_t grassIndex : grass.dirtyInstances) {
                            if (grassIndex < mapped->second.size())
                                uniqueIndices.insert(mapped->second[grassIndex]);
                        }
                    }
                }
                changedIndices.assign(uniqueIndices.begin(), uniqueIndices.end());
                std::ranges::sort(changedIndices);
            }

            const Registry& readRegistry = registry;
            std::vector<std::size_t> changedBatches;
            changedBatches.reserve(changedIndices.size());
            for (const std::size_t index : changedIndices) {
                const Entity entity = renderables[index].entity;
                if (!readRegistry.has<Transform>(entity)) {
                    continue;
                }
                const auto& transform = readRegistry.get<Transform>(entity);
                RenderableRecord& record = renderables[index];
                const bool grassInstance = record.grassInstance != std::numeric_limits<std::size_t>::max();
                if (!grassInstance && !readRegistry.has<MeshRenderer>(entity)) continue;
                if (grassInstance && (!readRegistry.has<TerrainGrassComponent>(entity) ||
                    record.grassInstance >= readRegistry.get<TerrainGrassComponent>(entity).instances.size())) continue;
                const MeshRenderer* renderer = grassInstance ? nullptr : &readRegistry.get<MeshRenderer>(entity);
                const TerrainGrassComponent* grass = grassInstance
                    ? &readRegistry.get<TerrainGrassComponent>(entity) : nullptr;
                Transform effectiveTransform = transform;
                glm::mat4 model = transform.matrix().native();
                if (grassInstance) {
                    const auto& item = grass->instances[record.grassInstance];
                    effectiveTransform = Transform{.position = item.position,
                        .rotation = Vec3{0.0F, item.yaw, 0.0F},
                        .scale = Vec3{item.scale, item.scale, item.scale}};
                    model *= effectiveTransform.matrix().native();
                }
                if (!optimizationFeatures.transformCaching ||
                    !record.hasCachedTransform || !sameTransform(record.cachedTransform, effectiveTransform) ||
                    grassInstance) {
                    instanceModels[index].model = model;
                    const glm::mat3 normalMatrix = glm::transpose(glm::inverse(glm::mat3{model}));
                    const float meshHeight = record.localBounds.max.y() - record.localBounds.min.y();
                    instanceModels[index].normalColumn0 = glm::vec4{
                        normalMatrix[0], grassInstance ? record.localBounds.min.y() : 0.0F};
                    instanceModels[index].normalColumn1 = glm::vec4{
                        normalMatrix[1], grassInstance && meshHeight > 1.0e-5F ? 1.0F / meshHeight : 0.0F};
                    instanceModels[index].normalColumn2 = glm::vec4{normalMatrix[2], 0.0F};
                    instanceModels[index].grassDeformation = grassInstance
                        ? glm::vec4{grass->instances[record.grassInstance].bendX,
                                    grass->instances[record.grassInstance].bendZ,
                                    grass->instances[record.grassInstance].trampled,
                                    0.0F}
                        : glm::vec4{};
                    record.cachedTransform = effectiveTransform;
                    record.hasCachedTransform = true;
                    markDirty(index, &RenderableRecord::transformDirtyFrames, dirtyTransforms);
                    markDirty(index, &RenderableRecord::cullingDirtyFrames, dirtyCullingObjects);
                    changedBatches.push_back(record.batchIndex);
                }
                const Mesh& mesh = grassInstance ? *grass->mesh : *renderer->mesh;
                bool materialChanged = false;
                for (std::uint32_t slot = 0; slot < materialSlots; ++slot) {
                    const PBRMaterial source = mesh.materials.empty()
                        ? (grassInstance ? grass->material : renderer->material)
                        : (slot < mesh.materials.size() ? mesh.materials[slot] : PBRMaterial{});
                    const auto textureIndex = [&](const std::int32_t localIndex) {
                        const auto offset = meshTextureOffsets.find(&mesh);
                        if (localIndex < 0 || offset == meshTextureOffsets.end() ||
                            static_cast<std::size_t>(localIndex) >= mesh.images.size()) return -1;
                        return static_cast<std::int32_t>(offset->second + localIndex);
                    };
                    const GPUMaterialData material{
                        glm::vec4{source.baseColor.r(), source.baseColor.g(),
                                  source.baseColor.b(), source.metallic},
                        glm::vec4{source.roughness, source.ambientOcclusion,
                                  source.alphaCutoff, source.normalScale},
                        glm::ivec4{textureIndex(source.baseColorTexture),
                                   textureIndex(source.metallicRoughnessTexture),
                                   textureIndex(source.normalTexture),
                                   (source.doubleSided ? 1 : 0) | (source.alphaBlend ? 2 : 0)},
                    };
                    GPUMaterialData& destination = materials[index * materialSlots + slot];
                    if (!optimizationFeatures.materialCaching ||
                        !sameMaterial(destination, material)) {
                        destination = material;
                        materialChanged = true;
                    }
                }
                if (materialChanged) {
                    markDirty(index, &RenderableRecord::materialDirtyFrames, dirtyMaterials);
                }
            }
            if (gpuObjects.size() == instanceBatches.size() && !changedBatches.empty()) {
                std::ranges::sort(changedBatches);
                changedBatches.erase(std::ranges::unique(changedBatches).begin(), changedBatches.end());
                for (const std::size_t batchIndex : changedBatches) {
                    if (batchIndex >= sceneGpu.batchRenderableIndices.size()) continue;
                    AABB bounds{};
                    bool initialized = false;
                    float deformationExpansion = 0.0F;
                    for (std::size_t index : sceneGpu.batchRenderableIndices[batchIndex]) {
                        const RenderableRecord& record = renderables[index];
                        if (!readRegistry.has<Transform>(record.entity)) continue;
                        const AABB worldBounds = record.localBounds.transformed(instanceModels[index].model);
                        if (record.grassInstance != std::numeric_limits<std::size_t>::max()) {
                            const glm::mat4& model = instanceModels[index].model;
                            const float scale = std::max(glm::length(glm::vec3{model[0]}),
                                                         glm::length(glm::vec3{model[2]}));
                            deformationExpansion = std::max(deformationExpansion,
                                (record.localBounds.max.y() - record.localBounds.min.y()) * scale * 0.8F);
                        }
                        if (!initialized) {
                            bounds = worldBounds;
                            initialized = true;
                        } else {
                            bounds.min = Vec3{std::min(bounds.min.x(), worldBounds.min.x()),
                                              std::min(bounds.min.y(), worldBounds.min.y()),
                                              std::min(bounds.min.z(), worldBounds.min.z())};
                            bounds.max = Vec3{std::max(bounds.max.x(), worldBounds.max.x()),
                                              std::max(bounds.max.y(), worldBounds.max.y()),
                                              std::max(bounds.max.z(), worldBounds.max.z())};
                        }
                    }
                    if (!initialized) continue;
                    bounds.min.setX(bounds.min.x() - deformationExpansion);
                    bounds.min.setZ(bounds.min.z() - deformationExpansion);
                    bounds.max.setX(bounds.max.x() + deformationExpansion);
                    bounds.max.setZ(bounds.max.z() + deformationExpansion);
                    instanceBatches[batchIndex].worldBounds = bounds;
                    auto& object = gpuObjects[batchIndex];
                    object.localAabbMin = {bounds.min.x(), bounds.min.y(), bounds.min.z(), 0.0F};
                    object.localAabbMax = {bounds.max.x(), bounds.max.y(), bounds.max.z(), 0.0F};
                    object.model = {};
                    object.model.data[0] = 1.0F;
                    object.model.data[5] = 1.0F;
                    object.model.data[10] = 1.0F;
                    object.model.data[15] = 1.0F;
                }
                for (Buffer& buffer : cullingObjectBuffers) {
                    if (buffer.handle() == VK_NULL_HANDLE) continue;
                    std::size_t rangeStart = 0;
                    while (rangeStart < changedBatches.size()) {
                        std::size_t rangeEnd = rangeStart + 1;
                        while (rangeEnd < changedBatches.size() &&
                               changedBatches[rangeEnd] == changedBatches[rangeEnd - 1] + 1) ++rangeEnd;
                        const std::size_t first = changedBatches[rangeStart];
                        buffer.update(gpuObjects.data() + first,
                                      sizeof(Culling::GPUObjectData) * (rangeEnd - rangeStart),
                                      sizeof(Culling::GPUObjectData) * first);
                        rangeStart = rangeEnd;
                    }
                }
            }
            uploadPendingRenderableBuffers();
            registry.view<TerrainGrassComponent>([](const Entity, const TerrainGrassComponent& grass) {
                grass.dirtyInstances.clear();
                grass.allInstancesDirty = false;
            });
            lastTransformRevision = transformRevision;
            lastMeshRendererRevision = meshRendererRevision;
            lastTerrainGrassRevision = terrainGrassRevision;
        }

        [[nodiscard]] bool canUseHiZOcclusionCulling() const noexcept {
            return optimizationFeatures.gpuCulling && optimizationFeatures.occlusionCulling;
        }

        void updateCullingUniformBuffer(const uint32_t frame) const {
            // Empty editor scenes intentionally do not allocate culling
            // resources. The render passes already skip zero-object draws,
            // so there is no uniform buffer to update in that case.
            if (cullingUniformBuffers[frame].handle() == VK_NULL_HANDLE) return;
            constexpr float hizDepthBias = 0.0025F;
            constexpr float hizAabbExpansion = 0.01F;
            Culling::CullingUniformData data{};
            if (!cameraController.camera()) {
                throw std::runtime_error("Camera must be initialized before culling");
            }
            const glm::mat4 viewProjection = cameraController.camera()->projectionMatrix().native() * cameraController.camera()->viewMatrix().native();
            std::memcpy(data.viewProjection.data, &viewProjection, sizeof(viewProjection));
            data.objectCount = static_cast<uint32_t>(gpuObjects.size());
            data.maxDrawCount = data.objectCount;
            data.hizMipCount = hiZBuffer.mipCount();
            data.enableOcclusionCulling = canUseHiZOcclusionCulling() ? 1U : 0U;
            data.viewportWidth = static_cast<float>(swapchain.extent().width);
            data.viewportHeight = static_cast<float>(swapchain.extent().height);
            data.depthBias = hizDepthBias;
            data.aabbExpansion = hizAabbExpansion;
            // Never reject objects using an uninitialized hierarchy.
            data.cameraCut = hiZValid ? 0u : 1u;
            data.shadowPass = 0;
            data.enableFrustumCulling = optimizationFeatures.gpuCulling ? 1u : 0u;
            cullingUniformBuffers[frame].update(&data, sizeof(data));
        }

        void updateSceneCullingUniformBuffer(const uint32_t frame) const {
            if (sceneCullingUniformBuffers[frame].handle() == VK_NULL_HANDLE) return;
            Culling::CullingUniformData data{};
            const float aspect = static_cast<float>(sceneViewportTarget.extent().width) /
                                 static_cast<float>(sceneViewportTarget.extent().height);
            Camera sceneCamera{Degrees{60.0F}, aspect, 0.1F, 1000.0F};
            sceneCamera.setPosition(cameraController.editorPosition());
            sceneCamera.setRotation(Degrees{cameraController.editorYaw()},
                                    Degrees{cameraController.editorPitch()});
            const glm::mat4 viewProjection = sceneCamera.projectionMatrix().native() * sceneCamera.viewMatrix().native();
            std::memcpy(data.viewProjection.data, &viewProjection, sizeof(viewProjection));
            data.objectCount = static_cast<uint32_t>(gpuObjects.size());
            data.maxDrawCount = data.objectCount;
            data.enableOcclusionCulling = 0;
            data.enableFrustumCulling = optimizationFeatures.gpuCulling ? 1U : 0U;
            data.cameraCut = 1;
            data.shadowPass = 0;
            sceneCullingUniformBuffers[frame].update(&data, sizeof(data));
        }

        void updateShadowCullingUniformBuffer(const uint32_t frame) const {
            if (shadowCullingUniformBuffers[frame].handle() == VK_NULL_HANDLE) return;
            constexpr float hizAabbExpansion = 0.01F;
            Culling::CullingUniformData data{};
            const glm::mat4 lightViewProjection = lightSpaceMatrix().native();
            std::memcpy(data.viewProjection.data, &lightViewProjection, sizeof(lightViewProjection));
            data.objectCount = static_cast<uint32_t>(gpuObjects.size());
            data.maxDrawCount = data.objectCount;
            // Shadow culling uses only the light frustum. Camera Hi-Z cannot safely
            // reject casters which are invisible to the camera but visible to the light.
            data.enableOcclusionCulling = 0;
            data.aabbExpansion = hizAabbExpansion;
            data.cameraCut = 1;
            data.shadowPass = 1;
            data.enableFrustumCulling = optimizationFeatures.gpuCulling ? 1u : 0u;
            shadowCullingUniformBuffers[frame].update(&data, sizeof(data));
        }

        void createUniformBuffers() {
            for (Buffer& buffer : uniformBuffers) {
                buffer.createHostVisible(vulkanDevice.physical(), device, sizeof(UniformBufferObject),
                                         VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, vulkanDevice.allocator());
            }
        }

        void createSceneUniformBuffers() {
            for (Buffer& buffer : sceneUniformBuffers) {
                buffer.createHostVisible(vulkanDevice.physical(), device, sizeof(UniformBufferObject),
                                         VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, vulkanDevice.allocator());
            }
        }

        VkPipeline createComputePipeline(const char* shaderPath, VkPipelineLayout layout) const {
            const auto shader = Vkutil::loadShaderModule(device, assetManager, shaderPath);
            VkPipelineShaderStageCreateInfo stage{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
            stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
            stage.module = shader.get();
            stage.pName = "main";
            VkComputePipelineCreateInfo info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
            info.stage = stage;
            info.layout = layout;
            VkPipeline pipeline = VK_NULL_HANDLE;
            if (vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline) != VK_SUCCESS) {
                throw std::runtime_error("Could not create Hi-Z compute pipeline");
            }
            return pipeline;
        }
