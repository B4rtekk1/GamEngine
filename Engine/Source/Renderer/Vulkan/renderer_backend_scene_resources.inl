        // A renderer topology rebuild is allowed to need decoded data, but it
        // must not make that decoded data resident between rebuilds. Imported
        // meshes are decoded again from their asset path into this short-lived
        // upload payload; procedural/edited meshes explicitly pin their data.
        void restoreMeshSourceDataForUpload() {
            registry.view<MeshRenderer>([&](const Entity, MeshRenderer& renderer) {
                if (!renderer.mesh || renderer.mesh.hasSourceData()) return;
                const auto resource = renderer.mesh.resource();
                if (!resource || resource->sourcePath.empty()) {
                    throw std::runtime_error("Mesh GPU resource has no source data or reloadable asset path");
                }
                const auto decoded = assetManager.reload<Mesh>(resource->sourcePath, Assets::AssetType::Mesh).shared();
                if (!decoded || decoded->empty()) {
                    throw std::runtime_error("Could not decode mesh source for GPU resource rebuild: " +
                                             resource->sourcePath.string());
                }
                resource->sourceData = decoded;
            });
        }

        void releaseUploadedMeshSourceData() {
            registry.view<MeshRenderer>([&](const Entity, MeshRenderer& renderer) {
                if (renderer.mesh.uploaded()) renderer.mesh.releaseSourceReference();
            });
            // Cache entries are deliberately weak from the renderer's point
            // of view. This is what returns decoded vertex/index/RGBA memory
            // to the allocator after the upload has completed.
            assetManager.unload_unused();
        }

        void createMaterialTextures() {
            restoreMeshSourceDataForUpload();
            auto uploadBatch = uploadContext.beginBatch();
            std::size_t rawImages = 0;
            std::size_t cookedImages = 0;
            std::size_t gtexImages = 0;
            VkDeviceSize rawBytes = 0;
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
                const auto source = renderer.mesh.source();
                if (!renderer.hasRenderableMesh() || !source || !uploaded.insert(source.get()).second) return;
                const Mesh& mesh = *source;
                const auto offset = static_cast<std::uint32_t>(materialTextures.size() + 1);
                if (mesh.images.size() > MaxMaterialTextures - offset) {
                    throw std::runtime_error("GLB scene exceeds the 4096-entry bindless texture capacity");
                }
                meshTextureOffsets.emplace(&mesh, offset);
                for (std::size_t i = 0; i < mesh.images.size(); ++i) {
                    const Mesh::Image& image = mesh.images[i];
                    const bool isColorTexture = std::ranges::any_of(
                        mesh.materials, [i](const PBRMaterial& material) {
                            const auto index = static_cast<std::int32_t>(i);
                            return material.baseColorTexture == index || material.emissiveTexture == index;
                        });
                    if (image.cooked) {
                        ++cookedImages;
                        Texture2D texture;
                        texture.createCooked(vulkanDevice.physical(), device, commandPool,
                                             vulkanDevice.graphicsQueue(), *image.cooked,
                                             vulkanDevice.allocator());
                        materialTextureDescriptors[offset + i] = {
                            texture.sampler(), texture.imageView(),
                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
                        materialTextures.push_back(std::move(texture));
                        continue;
                    }
                    if (image.gtex) {
                        ++gtexImages;
                        Texture2D texture;
                        texture.createGtex(vulkanDevice.physical(), device, commandPool,
                                           vulkanDevice.graphicsQueue(), *image.gtex,
                                           static_cast<std::uint32_t>(image.gtex->mips.size() - 1),
                                           vulkanDevice.allocator());
                        materialTextureDescriptors[offset + i] = {texture.sampler(), texture.imageView(),
                                                                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
                        materialTextures.push_back(std::move(texture));
                        continue;
                    }
                    if (image.width == 0 || image.height == 0 || image.rgbaPixels.empty()) {
                        materialTextures.emplace_back();
                        continue;
                    }
                    ++rawImages;
                    rawBytes += image.rgbaPixels.size();
                    Texture2D texture;
                    texture.create(vulkanDevice.physical(), device, commandPool,
                                   vulkanDevice.graphicsQueue(), image.width, image.height,
                                   image.rgbaPixels, isColorTexture ? TextureColorSpace::SRGB
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
                    throw std::runtime_error("Grass prefab exceeds the 4096-entry bindless texture capacity");
                }
                meshTextureOffsets.emplace(&mesh, offset);
                for (std::size_t i = 0; i < mesh.images.size(); ++i) {
                    const Mesh::Image& image = mesh.images[i];
                    const bool srgb = std::ranges::any_of(mesh.materials, [i](const PBRMaterial& material) {
                        const auto index = static_cast<std::int32_t>(i);
                        return material.baseColorTexture == index || material.emissiveTexture == index;
                    });
                    if (image.cooked) {
                        ++cookedImages;
                        Texture2D texture;
                        texture.createCooked(vulkanDevice.physical(), device, commandPool,
                                             vulkanDevice.graphicsQueue(), *image.cooked,
                                             vulkanDevice.allocator());
                        materialTextureDescriptors[offset + i] = {
                            texture.sampler(), texture.imageView(),
                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
                        materialTextures.push_back(std::move(texture));
                        continue;
                    }
                    if (image.gtex) {
                        ++gtexImages;
                        Texture2D texture;
                        texture.createGtex(vulkanDevice.physical(), device, commandPool,
                                           vulkanDevice.graphicsQueue(), *image.gtex,
                                           static_cast<std::uint32_t>(image.gtex->mips.size() - 1),
                                           vulkanDevice.allocator());
                        materialTextureDescriptors[offset + i] = {texture.sampler(), texture.imageView(),
                                                                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
                        materialTextures.push_back(std::move(texture));
                        continue;
                    }
                    if (image.width == 0 || image.height == 0 || image.rgbaPixels.empty()) {
                        materialTextures.emplace_back();
                        continue;
                    }
                    ++rawImages;
                    rawBytes += image.rgbaPixels.size();
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
            [[maybe_unused]] const UploadTicket ticket = uploadBatch.submit();
            const auto& stats = uploadContext.statistics();
            Diagnostics::instance().report(
                DiagnosticSeverity::Info,
                "[SceneSync] MaterialTextures complete: rawImages=" + std::to_string(rawImages) +
                ", rawBytes=" + std::to_string(rawBytes) + ", cookedImages=" +
                std::to_string(cookedImages) + ", gtexImages=" + std::to_string(gtexImages) +
                ", uploadBytes=" + std::to_string(stats.bytesUploaded) + ", forcedSubmits=" +
                std::to_string(stats.forcedSubmits) + ", ringWraps=" + std::to_string(stats.ringWraps) +
                ", waits=" + std::to_string(stats.waits),
                {.subsystem = "Renderer"});
            Diagnostics::instance().flush();
        }

        [[nodiscard]] GPUMaterialData packMaterial(const PBRMaterial& source,
                                                   const Mesh& mesh,
                                                   const WaterMaterial* water = nullptr) const {
            const auto textureIndex = [&](const std::int32_t localIndex) {
                const auto offset = meshTextureOffsets.find(&mesh);
                if (localIndex < 0 || offset == meshTextureOffsets.end() ||
                    static_cast<std::size_t>(localIndex) >= mesh.images.size()) return -1;
                return static_cast<std::int32_t>(offset->second + localIndex);
            };
            const int materialFlags = (source.doubleSided ? 1 : 0) |
                (static_cast<int>(source.alphaMode) << 1) | (source.terrainLayered ? 8 : 0) |
                (source.shadingModel == MaterialShadingModel::Foliage ? 16 : 0) |
                (source.vertexColorUsage == VertexColorUsage::FoliageData ? 32 : 0) |
                (source.hasSpecularExtension ? 64 : 0);
            const auto coordinateSet = [&](const MaterialTextureSlot slot) {
                const auto& value = source.textureTransforms[static_cast<std::size_t>(slot)];
                const bool identity = value.offsetX == 0.0F && value.offsetY == 0.0F &&
                                      value.scaleX == 1.0F && value.scaleY == 1.0F && value.rotation == 0.0F;
                return static_cast<int>(value.texCoord) | (identity ? 2 : 0);
            };
            const auto transform = [&](const MaterialTextureSlot slot) {
                const auto& value = source.textureTransforms[static_cast<std::size_t>(slot)];
                const float cosine = std::cos(value.rotation);
                const float sine = std::sin(value.rotation);
                return glm::vec4{cosine * value.scaleX, -sine * value.scaleY,
                                 value.offsetX, value.offsetY};
            };
            std::array<glm::vec4, 6> transformRows1{};
            for (std::size_t i = 0; i < source.textureTransforms.size(); ++i) {
                const auto& value = source.textureTransforms[i];
                const float sine = std::sin(value.rotation);
                const float cosine = std::cos(value.rotation);
                transformRows1[(i / 4) * 2][i % 4] = sine * value.scaleX;
                transformRows1[(i / 4) * 2 + 1][i % 4] = cosine * value.scaleY;
            }
            GPUMaterialData packed{
                glm::vec4{source.baseColor.r(), source.baseColor.g(), source.baseColor.b(), source.metallic},
                glm::vec4{source.roughness, source.aoStrength, source.alphaCutoff, source.baseColor.a()},
                glm::ivec4{textureIndex(source.baseColorTexture), textureIndex(source.metallicRoughnessTexture),
                           textureIndex(source.normalTexture), materialFlags},
                glm::ivec4{textureIndex(source.terrainLayerTextures[0]), textureIndex(source.terrainLayerTextures[1]),
                           textureIndex(source.terrainLayerTextures[2]), textureIndex(source.terrainLayerTextures[3])},
                glm::ivec4{textureIndex(source.aoTexture), textureIndex(source.opacityTexture),
                           textureIndex(source.translucencyTexture), textureIndex(source.displacementTexture)},
                glm::vec4{source.normalScale, source.translucency, source.displacementScale, source.specular},
                glm::vec4{source.displacementScale, source.displacementOffset, 0.0F, 0.0F},
                glm::ivec4{textureIndex(source.emissiveTexture), textureIndex(source.specularTexture),
                           textureIndex(source.specularColorTexture), -1},
                glm::vec4{source.specularColor.r(), source.specularColor.g(), source.specularColor.b(), 1.0F},
                glm::vec4{source.emissiveColor.r(), source.emissiveColor.g(), source.emissiveColor.b(),
                          std::max(0.0F, source.emissiveIntensity)},
                glm::ivec4{coordinateSet(MaterialTextureSlot::BaseColor), coordinateSet(MaterialTextureSlot::MetallicRoughness),
                           coordinateSet(MaterialTextureSlot::Normal), coordinateSet(MaterialTextureSlot::AmbientOcclusion)},
                glm::ivec4{coordinateSet(MaterialTextureSlot::Opacity), coordinateSet(MaterialTextureSlot::Translucency),
                           coordinateSet(MaterialTextureSlot::Displacement), coordinateSet(MaterialTextureSlot::Emissive)},
                glm::ivec4{coordinateSet(MaterialTextureSlot::Specular), coordinateSet(MaterialTextureSlot::SpecularColor), 0, 0},
                {transform(MaterialTextureSlot::BaseColor), transform(MaterialTextureSlot::MetallicRoughness),
                 transform(MaterialTextureSlot::Normal), transform(MaterialTextureSlot::AmbientOcclusion),
                 transform(MaterialTextureSlot::Opacity), transform(MaterialTextureSlot::Translucency),
                 transform(MaterialTextureSlot::Displacement), transform(MaterialTextureSlot::Emissive),
                 transform(MaterialTextureSlot::Specular), transform(MaterialTextureSlot::SpecularColor)},
                transformRows1,
            };
            if (water != nullptr) {
                const std::int32_t normalMap = textureIndex(water->normalMap);
                const std::int32_t foamTexture = textureIndex(water->foamTexture);
                const std::int32_t flowMap = textureIndex(water->flowMap);
                const std::int32_t flags = (water->enableSSR ? 1 : 0) |
                                           (water->enableCaustics ? 2 : 0) |
                                           (water->enableUnderwater ? 4 : 0);
                packed.waterShallowColorRoughness = {water->shallowColor.x(), water->shallowColor.y(),
                                                      water->shallowColor.z(), water->roughness};
                packed.waterDeepColorIor = {water->deepColor.x(), water->deepColor.y(),
                                             water->deepColor.z(), water->ior};
                packed.waterAbsorptionRefraction = {water->absorptionCoefficient.x(),
                                                     water->absorptionCoefficient.y(),
                                                     water->absorptionCoefficient.z(),
                                                     water->refractionStrength};
                packed.waterScatteringMaxDepth = {water->scatteringCoefficient.x(),
                                                   water->scatteringCoefficient.y(),
                                                   water->scatteringCoefficient.z(),
                                                   water->maxVisibleDepth};
                packed.waterFoam = {water->foamIntensity, water->foamThreshold,
                                    water->normalStrength, water->depthFadeDistance};
                packed.waterTextureIndices = {normalMap, foamTexture, flowMap, flags};
                packed.waterWaveCount.x = std::min(water->waveCount, 8U);
                for (std::uint32_t waveIndex = 0; waveIndex < packed.waterWaveCount.x; ++waveIndex) {
                    const auto& wave = water->waves[waveIndex];
                    packed.waterWaves[waveIndex] = {wave.direction.x(), wave.direction.y(), wave.amplitude, wave.wavelength};
                    packed.waterWaveMotion[waveIndex] = {wave.speed, wave.steepness, 0.0F, 0.0F};
                }
            }
            return packed;
        }

        void createMeshBuffers() {
            auto uploadBatch = uploadContext.beginBatch();
            // A full scene reload clears the indexed Geometry Heap before it
            // reaches this function. Its meshlet offsets are tied to those
            // vertex offsets, so discard the companion heap as one generation
            // too. Ordinary topology deltas keep both heaps intact.
            if (geometryHeapAllocations.empty() && vertexBuffer.handle() == VK_NULL_HANDLE) {
                geometryHeapMeshIds.clear();
                geometryHeapFreeVertices.clear();
                geometryHeapFreeIndices.clear();
                retiredGeometryHeapAllocations.clear();
                nextGeometryHeapMeshId = 0;
                meshletBuffer.destroy();
                meshletClusterBuffer.destroy();
                meshletVertexBuffer.destroy();
                meshletTriangleBuffer.destroy();
                meshletHeapAllocations.clear();
                meshletHeapFreeRanges.clear();
                meshletClusterHeapFreeRanges.clear();
                meshletVertexHeapFreeRanges.clear();
                meshletTriangleHeapFreeRanges.clear();
                retiredMeshletHeapAllocations.clear();
                meshletHeapHighWater = 0;
                meshletClusterHeapHighWater = 0;
                meshletVertexHeapHighWater = 0;
                meshletTriangleHeapHighWater = 0;
                globalMeshletCount = 0;
            }
            // Renderable tables are reconstructed below, but geometry itself
            // is retained in the append-only Geometry Heap.  In particular,
            // do not clear GPUSceneDatabase here: existing proxies retain
            // their instance/mesh/material IDs across an ECS topology delta.
            for (const RenderableRecord& record : renderables) {
                if (!registry.has<Transform>(record.entity) ||
                    !registry.has<MeshRenderer>(record.entity) ||
                    !registry.get<MeshRenderer>(record.entity).hasMesh()) {
                    sceneGpu.database.removeInstance(static_cast<std::uint64_t>(record.entity),
                                                     submittedFrameValue);
                }
            }
            renderables.reserve(registry.size());
            renderables.clear();
            instanceBatches.clear();
            sceneGpu.batchRenderableIndices.clear();
            sceneGpu.renderableIndices.clear();
            sceneGpu.grassInstances.clear();
            sceneGpu.grassDeformations.clear();
            sceneGpu.grassInstanceGpuIndices.clear();
            sceneGpu.grassClusters.clear();
            sceneGpu.grassClusterEntities.clear();
            previousInstanceTransforms.clear();
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
            struct MeshUpload {
                const Mesh* mesh;
                std::uint32_t firstVertex;
                std::uint32_t firstIndex;
            };
            struct BatchKey {
                const void* mesh;
                std::uint32_t sectionIndex;
                std::uint32_t shaderSlot;
                bool foliagePipeline;
                bool castShadow;
                ShadowCacheMode shadowCacheMode;
                uint32_t cullingBatch;

                bool operator==(const BatchKey& other) const noexcept {
                    return mesh == other.mesh && sectionIndex == other.sectionIndex && shaderSlot == other.shaderSlot &&
                           foliagePipeline == other.foliagePipeline && castShadow == other.castShadow &&
                           shadowCacheMode == other.shadowCacheMode && cullingBatch == other.cullingBatch;
                }
            };
            struct BatchKeyHash {
                std::size_t operator()(const BatchKey& key) const noexcept {
                    constexpr std::uint32_t hashCombineConstant = 0x9e3779b9U;
                    constexpr std::uint32_t hashCombineLeftShift = 6U;
                    const auto meshHash = std::hash<const void*>{}(key.mesh);
                    const auto sectionHash = std::hash<std::uint32_t>{}(key.sectionIndex);
                    const auto batchHash = std::hash<uint32_t>{}(key.cullingBatch);
                    const auto shaderHash = std::hash<std::uint32_t>{}(key.shaderSlot);
                    return meshHash ^ (sectionHash + batchHash + shaderHash + static_cast<std::size_t>(key.foliagePipeline) +
                                       static_cast<std::size_t>(key.castShadow) +
                                       (static_cast<std::size_t>(key.shadowCacheMode) << 3U) +
                                       hashCombineConstant + (meshHash << hashCombineLeftShift) +
                                       (meshHash >> 2U));
                }
            };
            std::unordered_map<BatchKey, std::size_t, BatchKeyHash> batchIndices;
            batchIndices.reserve(registry.size());
            materialSlots = 1;
            std::unordered_set<const Mesh*> uniqueMeshes;
            std::unordered_map<const Mesh*, const void*> meshResources;
            uniqueMeshes.reserve(registry.size());
            meshResources.reserve(registry.size());
            registry.view<MeshRenderer>([&](const Entity, const MeshRenderer& renderer) {
                const auto source = renderer.mesh.source();
                const auto resource = renderer.mesh.resource();
                if (!renderer.hasRenderableMesh() || !source || !resource || !uniqueMeshes.insert(source.get()).second) {
                    return;
                }
                meshResources.emplace(source.get(), resource.get());
                materialSlots = std::max(materialSlots, static_cast<std::uint32_t>(
                    std::max<std::size_t>(1, source->materials.size())));
            });
            registry.view<TerrainGrassComponent>([&](const Entity, const TerrainGrassComponent& grass) {
                if (!grass.hasPrefab() || !uniqueMeshes.insert(grass.mesh.get()).second) return;
                // Grass still owns an immutable shared Mesh directly; unlike
                // imported MeshRenderer assets it is not source-reloaded.
                meshResources.emplace(grass.mesh.get(), grass.mesh.get());
                materialSlots = std::max(materialSlots, static_cast<std::uint32_t>(
                    std::max<std::size_t>(1, grass.mesh->materials.size())));
            });
            // Reclaim only ranges whose last possible GPU use has completed.
            // This runs after acquireFrameImage(), where completedFrameValue is
            // advanced from the fence for the recycled frame slot.
            const auto insertFreeRange = [](std::vector<GeometryHeapRange>& ranges,
                                            const GeometryHeapRange range) {
                if (range.count == 0U) return;
                ranges.push_back(range);
                std::sort(ranges.begin(), ranges.end(), [](const auto& left, const auto& right) {
                    return left.first < right.first;
                });
                std::vector<GeometryHeapRange> merged;
                merged.reserve(ranges.size());
                for (const GeometryHeapRange current : ranges) {
                    if (!merged.empty() && current.first <= merged.back().first + merged.back().count) {
                        merged.back().count = std::max(merged.back().count,
                            current.first + current.count - merged.back().first);
                    } else {
                        merged.push_back(current);
                    }
                }
                ranges = std::move(merged);
            };
            for (auto retired = retiredGeometryHeapAllocations.begin();
                 retired != retiredGeometryHeapAllocations.end();) {
                if (retired->reclaimAfter > completedFrameValue) {
                    ++retired;
                    continue;
                }
                insertFreeRange(geometryHeapFreeVertices,
                                {retired->allocation.firstVertex, retired->allocation.vertexCount});
                insertFreeRange(geometryHeapFreeIndices,
                                {retired->allocation.firstIndex, retired->allocation.indexCount});
                retired = retiredGeometryHeapAllocations.erase(retired);
            }
            // An unloaded resource may have no remaining ECS owner. Retire
            // its physical range now, but retain it through the in-flight
            // frame. Its MeshId is deliberately not tied to either offset.
            std::unordered_set<const void*> liveGeometryResources;
            liveGeometryResources.reserve(meshResources.size());
            for (const auto& [mesh, resource] : meshResources) {
                static_cast<void>(mesh);
                liveGeometryResources.insert(resource);
            }
            for (auto allocation = geometryHeapAllocations.begin(); allocation != geometryHeapAllocations.end();) {
                if (liveGeometryResources.contains(allocation->first)) {
                    ++allocation;
                    continue;
                }
                retiredGeometryHeapAllocations.push_back({allocation->second, submittedFrameValue});
                allocation = geometryHeapAllocations.erase(allocation);
            }
            for (auto retired = retiredMeshletHeapAllocations.begin();
                 retired != retiredMeshletHeapAllocations.end();) {
                if (retired->reclaimAfter > completedFrameValue) {
                    ++retired;
                    continue;
                }
                const MeshletHeapAllocation& allocation = retired->allocation;
                insertFreeRange(meshletHeapFreeRanges, {allocation.firstMeshlet, allocation.meshletCount});
                insertFreeRange(meshletClusterHeapFreeRanges, {allocation.firstCluster, allocation.clusterCount});
                insertFreeRange(meshletVertexHeapFreeRanges, {allocation.firstVertexIndex, allocation.vertexIndexCount});
                insertFreeRange(meshletTriangleHeapFreeRanges, {allocation.firstTriangle, allocation.triangleCount});
                retired = retiredMeshletHeapAllocations.erase(retired);
            }
            for (auto allocation = meshletHeapAllocations.begin(); allocation != meshletHeapAllocations.end();) {
                if (liveGeometryResources.contains(allocation->first)) {
                    ++allocation;
                    continue;
                }
                retiredMeshletHeapAllocations.push_back({allocation->second, submittedFrameValue});
                allocation = meshletHeapAllocations.erase(allocation);
            }
            // Plan every final range before allocating GPU memory.  Keeping only
            // these records avoids a second, scene-sized CPU Mesh during upload.
            std::vector<MeshUpload> meshUploads;
            meshUploads.reserve(uniqueMeshes.size());
            std::unordered_map<Entity, MeshUploadRecord> rendererUploads;
            std::unordered_map<Entity, MeshUploadRecord> grassUploads;
            std::unordered_map<const void*, MeshUploadRecord> plannedUploadedMeshes;
            rendererUploads.reserve(registry.size());
            grassUploads.reserve(registry.size());
            plannedUploadedMeshes.reserve(uniqueMeshes.size());
            std::uint32_t vertexCount = geometryHeapVertexHighWater;
            std::uint32_t indexCount = geometryHeapIndexHighWater;
            const auto allocateRange = [](std::vector<GeometryHeapRange>& freeRanges,
                                          std::uint32_t& highWater, const std::uint32_t count) {
                for (auto range = freeRanges.begin(); range != freeRanges.end(); ++range) {
                    if (range->count < count) continue;
                    const std::uint32_t first = range->first;
                    range->first += count;
                    range->count -= count;
                    if (range->count == 0U) freeRanges.erase(range);
                    return first;
                }
                if (count > std::numeric_limits<std::uint32_t>::max() - highWater)
                    throw std::runtime_error("Scene geometry exceeds 32-bit draw limits");
                const std::uint32_t first = highWater;
                highWater += count;
                return first;
            };
            const auto planUpload = [&](const Mesh* mesh) {
                const auto resourceIt = meshResources.find(mesh);
                if (resourceIt == meshResources.end() || resourceIt->second == nullptr)
                    throw std::runtime_error("Mesh has no stable GPU resource identity");
                const void* const key = resourceIt->second;
                if (!geometryHeapMeshIds.contains(key)) {
                    if (nextGeometryHeapMeshId == MeshId::Invalid)
                        throw std::runtime_error("Geometry mesh ID space exhausted");
                    geometryHeapMeshIds.emplace(key, MeshId{nextGeometryHeapMeshId++});
                }
                if (const auto found = geometryHeapAllocations.find(key);
                    found != geometryHeapAllocations.end() &&
                    found->second.vertexCount == mesh->vertices.size() &&
                    found->second.indexCount == mesh->indices.size()) {
                    return MeshUploadRecord{found->second.firstIndex, found->second.firstVertex, {}};
                }
                if (const auto previous = geometryHeapAllocations.find(key);
                    previous != geometryHeapAllocations.end()) {
                    retiredGeometryHeapAllocations.push_back({previous->second, submittedFrameValue});
                    geometryHeapAllocations.erase(previous);
                }
                if (mesh->vertices.size() > std::numeric_limits<std::uint32_t>::max() ||
                    mesh->indices.size() > std::numeric_limits<std::uint32_t>::max()) {
                    throw std::runtime_error("Scene geometry exceeds 32-bit draw limits");
                }
                const auto vertices = static_cast<std::uint32_t>(mesh->vertices.size());
                const auto indices = static_cast<std::uint32_t>(mesh->indices.size());
                const MeshUploadRecord record{allocateRange(geometryHeapFreeIndices, indexCount, indices),
                                              allocateRange(geometryHeapFreeVertices, vertexCount, vertices), {}};
                meshUploads.push_back({mesh, record.firstVertex, record.firstIndex});
                geometryHeapAllocations[key] = {
                    .firstVertex = record.firstVertex,
                    .vertexCount = vertices,
                    .firstIndex = record.firstIndex,
                    .indexCount = indices,
                };
                return record;
            };
            registry.view<Transform, MeshRenderer>([&](const Entity entity, const Transform&, const MeshRenderer& renderer) {
                const auto source = renderer.mesh.source();
                const MeshGpuResource* const resource = renderer.mesh.resource().get();
                if (!renderer.hasRenderableMesh() || !source || resource == nullptr) return;
                const Mesh* const mesh = source.get();
                MeshUploadRecord record{};
                if (optimizationFeatures.meshDeduplication) {
                    if (const auto found = plannedUploadedMeshes.find(resource); found != plannedUploadedMeshes.end()) {
                        record = found->second;
                    } else {
                        record = planUpload(mesh);
                        plannedUploadedMeshes.emplace(resource, record);
                    }
                } else {
                    record = planUpload(mesh);
                }
                rendererUploads.emplace(entity, record);
            });
            registry.view<Transform, TerrainGrassComponent>([&](const Entity entity, const Transform&, const TerrainGrassComponent& grass) {
                if (!grass.hasPrefab() || grass.instances.empty()) return;
                const Mesh* const mesh = grass.mesh.get();
                const void* const resource = grass.mesh.get();
                MeshUploadRecord record{};
                if (const auto found = plannedUploadedMeshes.find(resource); found != plannedUploadedMeshes.end()) {
                    record = found->second;
                } else {
                    record = planUpload(mesh);
                    plannedUploadedMeshes.emplace(resource, record);
                }
                grassUploads.emplace(entity, record);
            });
            geometryHeapVertexHighWater = vertexCount;
            geometryHeapIndexHighWater = indexCount;
            // Water contributes a transform slot to the shared scene-instance
            // buffer, but deliberately contributes no generic draw record.
            // VirtualWaterRenderer is its sole geometry owner. This retains
            // the existing shader descriptor contract while removing the ECS
            // MeshRenderer requirement.
            registry.view<Transform, WaterBodyComponent>(
                [&](const Entity entity, const Transform&, const WaterBodyComponent&) {
                    const std::size_t instanceIndex = renderables.size();
                    renderables.push_back({
                        .entity = entity,
                        .localBounds = {{-Water::OceanExtents.back(), -2.0F, -Water::OceanExtents.back()},
                                        { Water::OceanExtents.back(),  2.0F,  Water::OceanExtents.back()}},
                        .batchIndex = 0,
                        .firstVertex = 0,
                        .vertexCount = 0,
                        .waterOnly = true,
                    });
                    sceneGpu.renderableIndices[entity].push_back(instanceIndex);
                });
            if (vertexCount == 0 || indexCount == 0) {
                // The empty-scene path below keeps valid dummy bindings.
            } else {
                const auto growCapacity = [](const std::uint32_t required) {
                    // Leave headroom for normal editor additions. Growth is
                    // deliberately rare; it is the only point at which the
                    // heap must be reallocated.
                    return std::max(required, required + required / 2U + 1U);
                };
                const VkDeviceSize requiredVertexBytes = sizeof(GpuVertex) * static_cast<VkDeviceSize>(vertexCount);
                const VkDeviceSize requiredIndexBytes = sizeof(std::uint32_t) * static_cast<VkDeviceSize>(indexCount);
                const bool growVertexHeap = vertexBuffer.handle() == VK_NULL_HANDLE ||
                    vertexBuffer.size() < requiredVertexBytes;
                const bool growIndexHeap = indexBuffer.handle() == VK_NULL_HANDLE ||
                    indexBuffer.size() < requiredIndexBytes;
                if (growVertexHeap) {
                    vertexBuffer.createDeviceLocalEmpty(device,
                        sizeof(GpuVertex) * static_cast<VkDeviceSize>(growCapacity(vertexCount)),
                        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                        (vulkanDevice.supportsRayQuery()
                            ? VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                              VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT
                            : 0),
                        vulkanDevice.allocator(), vulkanDevice.supportsRayQuery());
                }
                if (growIndexHeap) {
                    indexBuffer.createDeviceLocalEmpty(device,
                        sizeof(std::uint32_t) * static_cast<VkDeviceSize>(growCapacity(indexCount)),
                        VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                        (vulkanDevice.supportsRayQuery()
                            ? VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                              VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT
                            : 0),
                        vulkanDevice.allocator(), vulkanDevice.supportsRayQuery());
                }
                if (growVertexHeap || growIndexHeap) {
                    // A newly allocated heap has no contents from the old
                    // backing allocation.  Reupload every live allocation,
                    // but keep each allocation's immutable offsets.
                    meshUploads.clear();
                    meshUploads.reserve(uniqueMeshes.size());
                    for (const Mesh* mesh : uniqueMeshes) {
                        // uniqueMeshes also contains mesh components that are
                        // not a draw this frame (for example an entity without
                        // Transform, or an empty grass prefab). Such a mesh
                        // was deliberately not passed to planUpload(), so it
                        // has no heap allocation to restore.
                        const auto resourceIt = meshResources.find(mesh);
                        if (resourceIt == meshResources.end()) continue;
                        const auto allocationIt = geometryHeapAllocations.find(resourceIt->second);
                        if (allocationIt == geometryHeapAllocations.end()) continue;
                        const GeometryHeapAllocation& allocation = allocationIt->second;
                        meshUploads.push_back({mesh, allocation.firstVertex, allocation.firstIndex});
                    }
                }
                for (const MeshUpload& upload : meshUploads) {
                    std::vector<GpuVertex> packedVertices;
                    packedVertices.reserve(upload.mesh->vertices.size());
                    for (const Vertex& vertex : upload.mesh->vertices) {
                        packedVertices.push_back(GpuVertex::pack(vertex));
                    }
                    vertexBuffer.uploadDeviceLocal(packedVertices.data(),
                        sizeof(GpuVertex) * packedVertices.size(), sizeof(GpuVertex) * upload.firstVertex,
                        commandPool, vulkanDevice.graphicsQueue());
                    const auto indicesPerChunk = static_cast<std::size_t>(uploadContext.capacity() / sizeof(std::uint32_t));
                    for (std::size_t first = 0; first < upload.mesh->indices.size(); first += indicesPerChunk) {
                        const auto count = std::min(indicesPerChunk, upload.mesh->indices.size() - first);
                        const auto bytes = static_cast<VkDeviceSize>(sizeof(std::uint32_t) * count);
                        const auto slice = uploadContext.allocate(bytes, alignof(std::uint32_t));
                        auto* const indices = static_cast<std::uint32_t*>(slice.mapped);
                        for (std::size_t i = 0; i < count; ++i)
                            indices[i] = upload.firstVertex + upload.mesh->indices[first + i];
                        indexBuffer.copyFromUploadRing(slice.buffer, slice.offset, bytes,
                            sizeof(std::uint32_t) * (upload.firstIndex + first));
                    }
                }
            }
            // Meshlet streams intentionally use global vertex references so a
            // mesh shader can fetch the same packed vertex heap as the indexed
            // fallback. Their physical sub-allocations are reclaimable after
            // the frame fence, just like indexed geometry.
            struct MeshletUpload {
                const Mesh* mesh{};
                GeometryHeapAllocation geometry{};
                MeshletHeapAllocation allocation{};
            };
            std::vector<MeshletUpload> meshletUploads;
            std::unordered_map<const void*, std::uint32_t> firstMeshlets;
            std::unordered_map<const void*, std::uint32_t> firstMeshletClusters;
            const auto allocateMeshletRange = [](std::vector<GeometryHeapRange>& freeRanges,
                                                 std::uint32_t& highWater, const std::uint32_t count) {
                for (auto range = freeRanges.begin(); range != freeRanges.end(); ++range) {
                    if (range->count < count) continue;
                    const std::uint32_t first = range->first;
                    range->first += count;
                    range->count -= count;
                    if (range->count == 0U) freeRanges.erase(range);
                    return first;
                }
                if (count > std::numeric_limits<std::uint32_t>::max() - highWater)
                    throw std::runtime_error("Meshlet heap exceeds 32-bit shader addressing limits");
                const std::uint32_t first = highWater;
                highWater += count;
                return first;
            };
            firstMeshlets.reserve(geometryHeapAllocations.size());
            firstMeshletClusters.reserve(geometryHeapAllocations.size());
            meshletUploads.reserve(uniqueMeshes.size());
            for (const Mesh* mesh : uniqueMeshes) {
                const auto resourceIt = meshResources.find(mesh);
                if (resourceIt == meshResources.end()) continue;
                const auto geometry = geometryHeapAllocations.find(resourceIt->second);
                if (geometry == geometryHeapAllocations.end() || mesh->meshlets.empty()) continue;
                const auto payloadMatches = [mesh](const MeshletHeapAllocation& allocation) {
                    return allocation.meshletCount == mesh->meshlets.size() &&
                           allocation.clusterCount == mesh->meshletClusters.size() &&
                           allocation.vertexIndexCount == mesh->meshletVertices.size() &&
                           allocation.triangleCount == mesh->meshletTriangles.size();
                };
                auto allocationIt = meshletHeapAllocations.find(resourceIt->second);
                const bool needsUpload = allocationIt == meshletHeapAllocations.end() ||
                                         !payloadMatches(allocationIt->second);
                if (needsUpload) {
                    if (allocationIt != meshletHeapAllocations.end()) {
                        retiredMeshletHeapAllocations.push_back({allocationIt->second, submittedFrameValue});
                        meshletHeapAllocations.erase(allocationIt);
                    }
                    if (mesh->meshlets.size() > std::numeric_limits<std::uint32_t>::max() ||
                        mesh->meshletClusters.size() > std::numeric_limits<std::uint32_t>::max() ||
                        mesh->meshletVertices.size() > std::numeric_limits<std::uint32_t>::max() ||
                        mesh->meshletTriangles.size() > std::numeric_limits<std::uint32_t>::max()) {
                        throw std::runtime_error("Meshlet heap exceeds 32-bit shader addressing limits");
                    }
                    const MeshletHeapAllocation allocation{
                        .firstMeshlet = allocateMeshletRange(meshletHeapFreeRanges, meshletHeapHighWater,
                                                             static_cast<std::uint32_t>(mesh->meshlets.size())),
                        .meshletCount = static_cast<std::uint32_t>(mesh->meshlets.size()),
                        .firstCluster = allocateMeshletRange(meshletClusterHeapFreeRanges, meshletClusterHeapHighWater,
                                                             static_cast<std::uint32_t>(mesh->meshletClusters.size())),
                        .clusterCount = static_cast<std::uint32_t>(mesh->meshletClusters.size()),
                        .firstVertexIndex = allocateMeshletRange(meshletVertexHeapFreeRanges, meshletVertexHeapHighWater,
                                                                 static_cast<std::uint32_t>(mesh->meshletVertices.size())),
                        .vertexIndexCount = static_cast<std::uint32_t>(mesh->meshletVertices.size()),
                        .firstTriangle = allocateMeshletRange(meshletTriangleHeapFreeRanges, meshletTriangleHeapHighWater,
                                                             static_cast<std::uint32_t>(mesh->meshletTriangles.size())),
                        .triangleCount = static_cast<std::uint32_t>(mesh->meshletTriangles.size()),
                    };
                    allocationIt = meshletHeapAllocations.insert_or_assign(resourceIt->second, allocation).first;
                }
                const MeshletHeapAllocation& allocation = allocationIt->second;
                firstMeshlets.emplace(resourceIt->second, allocation.firstMeshlet);
                firstMeshletClusters.emplace(resourceIt->second, allocation.firstCluster);
                if (needsUpload) meshletUploads.push_back({mesh, geometry->second, allocation});
            }
            globalMeshletCount = meshletHeapHighWater;
            const auto growCapacity = [](const std::uint32_t required) {
                return std::max(1U, required + required / 2U + 1U);
            };
            const auto ensureMeshletHeap = [&](Buffer& buffer, const std::uint32_t required,
                                               const VkDeviceSize elementSize) {
                const VkDeviceSize requiredBytes = elementSize * std::max(1U, required);
                if (buffer.handle() != VK_NULL_HANDLE && buffer.size() >= requiredBytes) return false;
                buffer.createDeviceLocalEmpty(device, elementSize * growCapacity(required),
                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, vulkanDevice.allocator());
                return true;
            };
            const bool meshletHeapReallocated =
                ensureMeshletHeap(meshletBuffer, meshletHeapHighWater, sizeof(Culling::GpuMeshlet)) |
                ensureMeshletHeap(meshletClusterBuffer, meshletClusterHeapHighWater, sizeof(Culling::GpuMeshletCluster)) |
                ensureMeshletHeap(meshletVertexBuffer, meshletVertexHeapHighWater, sizeof(std::uint32_t)) |
                ensureMeshletHeap(meshletTriangleBuffer, meshletTriangleHeapHighWater, sizeof(std::uint32_t));
            if (meshletHeapReallocated) {
                // A capacity increase replaces all four backing buffers. Their
                // stable offsets remain valid, but every live allocation must
                // be restored once into the enlarged heap.
                meshletUploads.clear();
                meshletUploads.reserve(uniqueMeshes.size());
                for (const Mesh* mesh : uniqueMeshes) {
                    const auto resource = meshResources.find(mesh);
                    const auto geometry = resource == meshResources.end() ? geometryHeapAllocations.end() :
                        geometryHeapAllocations.find(resource->second);
                    const auto allocation = resource == meshResources.end() ? meshletHeapAllocations.end() :
                        meshletHeapAllocations.find(resource->second);
                    if (geometry != geometryHeapAllocations.end() && allocation != meshletHeapAllocations.end() &&
                        !mesh->meshlets.empty()) meshletUploads.push_back({mesh, geometry->second, allocation->second});
                }
            }
            for (const MeshletUpload& upload : meshletUploads) {
                std::vector<Culling::GpuMeshlet> meshlets;
                std::vector<Culling::GpuMeshletCluster> clusters;
                std::vector<std::uint32_t> vertices;
                std::vector<std::uint32_t> triangles;
                meshlets.reserve(upload.allocation.meshletCount);
                clusters.reserve(upload.allocation.clusterCount);
                vertices.reserve(upload.allocation.vertexIndexCount);
                triangles.reserve(upload.allocation.triangleCount);
                if (!Culling::appendMeshletPayload(*upload.mesh, upload.geometry.firstVertex, meshlets, vertices, triangles) ||
                    !Culling::appendMeshletClusterPayload(*upload.mesh, upload.allocation.firstMeshlet, clusters) ||
                    meshlets.size() != upload.allocation.meshletCount || clusters.size() != upload.allocation.clusterCount ||
                    vertices.size() != upload.allocation.vertexIndexCount || triangles.size() != upload.allocation.triangleCount) {
                    throw std::runtime_error("Invalid meshlet payload during GPU scene upload");
                }
                // appendMeshletPayload/ClusterPayload are also used by the
                // cooker and therefore append relative to their destination.
                // Rebase this standalone upload into the persistent heap.
                for (Culling::GpuMeshlet& meshlet : meshlets) {
                    meshlet.range.x += upload.allocation.firstVertexIndex;
                    meshlet.range.z += upload.allocation.firstTriangle;
                }
                for (Culling::GpuMeshletCluster& cluster : clusters)
                    cluster.range.x += upload.allocation.firstCluster;
                meshletBuffer.uploadDeviceLocal(meshlets.data(), sizeof(Culling::GpuMeshlet) * meshlets.size(),
                    sizeof(Culling::GpuMeshlet) * upload.allocation.firstMeshlet, commandPool, vulkanDevice.graphicsQueue());
                meshletClusterBuffer.uploadDeviceLocal(clusters.data(), sizeof(Culling::GpuMeshletCluster) * clusters.size(),
                    sizeof(Culling::GpuMeshletCluster) * upload.allocation.firstCluster, commandPool, vulkanDevice.graphicsQueue());
                meshletVertexBuffer.uploadDeviceLocal(vertices.data(), sizeof(std::uint32_t) * vertices.size(),
                    sizeof(std::uint32_t) * upload.allocation.firstVertexIndex, commandPool, vulkanDevice.graphicsQueue());
                meshletTriangleBuffer.uploadDeviceLocal(triangles.data(), sizeof(std::uint32_t) * triangles.size(),
                    sizeof(std::uint32_t) * upload.allocation.firstTriangle, commandPool, vulkanDevice.graphicsQueue());
            }
            // Descriptors change only on heap growth, never for an ordinary
            // streamed asset upload.
            if (meshletHeapReallocated && cullingDescriptorPool != VK_NULL_HANDLE) {
                for (std::uint32_t frame = 0; frame < MAX_FRAMES_IN_FLIGHT; ++frame)
                    refreshGPUSceneDescriptors(frame);
            }
            registry.view<Transform, MeshRenderer>(
                [&](const Entity entity, const Transform&, MeshRenderer& renderer) {
                    const auto source = renderer.mesh.source();
                    if (!renderer.hasRenderableMesh() || !source) {
                        return;
                    }

                    const Mesh* const mesh = source.get();
                    const MeshUploadRecord& planned = rendererUploads.at(entity);
                    renderer.firstIndex = planned.firstIndex;
                    const std::uint32_t firstVertex = planned.firstVertex;
                    AABB localBounds{
                        .min = Vec3{std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
                                    std::numeric_limits<float>::max()},
                        .max = Vec3{std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest(),
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
                    // Persist the GPU range on the ECS-facing handle.  The
                    // draw path can therefore stop consulting MeshSourceData
                    // once all batches use MeshGpuResource directly.
                    if (const auto& resource = renderer.mesh.resource()) {
                        resource->handle = geometryHeapMeshIds.at(resource.get());
                        resource->firstVertex = firstVertex;
                        resource->vertexCount = mesh->vertexCount();
                        resource->firstIndex = renderer.firstIndex;
                        resource->indexCount = mesh->indexCount();
                        resource->meshletCount = static_cast<std::uint32_t>(mesh->meshlets.size());
                        resource->firstMeshlet = firstMeshlets.contains(resource.get()) ? firstMeshlets.at(resource.get()) : 0U;
                        resource->bounds = localBounds;
                    }
                    // Culling must use the same parent-composed matrix as the
                    // instance renderer. Otherwise a child can be rendered at
                    // its parent's position but culled at its local position.
                    const AABB worldBounds = localBounds.transformed(worldModel(entity));
                    const bool castShadow = renderer.castShadow;
                    const bool overrideUsesFoliagePipeline = renderer.materialOverride &&
                        (renderer.material.pbr.doubleSided ||
                         renderer.material.pbr.alphaMode == AlphaMode::Mask ||
                         renderer.material.pbr.alphaMode == AlphaMode::Blend);
                    const auto resolveShaderSlot = [&]() {
                        if (renderer.material.shaderSource == MaterialShaderSource::BuiltIn) {
                            return static_cast<std::uint32_t>(materialShaderIndex(renderer.material.shader));
                        }
                        // An incomplete or failed Shader Graph keeps its authoring state but
                        // renders with a safe built-in fallback until it cooks successfully.
                        if (renderer.material.shaderProgram == 0) {
                            return static_cast<std::uint32_t>(materialShaderIndex(MaterialShader::StandardPBR));
                        }
                        if (renderer.material.shaderProgramSpirv.empty()) throw std::runtime_error("Shader Graph material has no cooked SPIR-V module");
                        const ShaderGraphProgram program{
                            renderer.material.shaderProgram, {}, renderer.material.shaderProgramSpirv};
                        const std::uint32_t shaderSlot = forwardPass.registerShaderGraph(
                            program, renderer.material.renderState);
                        static_cast<void>(lightingForwardPass.registerShaderGraph(
                            program, renderer.material.renderState));
                        // In the single-sample editor path Scene View owns a
                        // cache-compatible ForwardPass. It needs the same graph
                        // pipeline registry as Game View rather than falling
                        // back when an authored material is visible only there.
                        if (!msaa.enabled()) {
                            static_cast<void>(sceneViewportForwardPass.registerShaderGraph(
                                program, renderer.material.renderState));
                        }
                        return shaderSlot;
                    };
                    const std::uint32_t shaderSlot = resolveShaderSlot();
                    const auto pbrShaderSlot = [&](const PBRMaterial& material) -> std::uint32_t {
                        if (shaderSlot != materialShaderIndex(MaterialShader::StandardPBR))
                            return shaderSlot;
                        if (material.terrainLayered)
                            return static_cast<std::uint32_t>(PbrTerrainProgramSlot);
                        if (material.shadingModel == MaterialShadingModel::Foliage)
                            return static_cast<std::uint32_t>(PbrFoliageProgramSlot);
                        if (material.hasSpecularExtension)
                            return static_cast<std::uint32_t>(PbrExtendedProgramSlot);
                        if (material.normalTexture >= 0 && material.normalScale != 0.0F)
                            return static_cast<std::uint32_t>(PbrNormalProgramSlot);
                        return shaderSlot;
                    };
                    const bool virtualOcean = renderer.materialOverride &&
                        renderer.material.shaderSource == MaterialShaderSource::BuiltIn &&
                        renderer.material.shader == MaterialShader::Water &&
                        registry.has<WaterBodyComponent>(entity) &&
                        registry.get<WaterBodyComponent>(entity).type == WaterBodyType::Ocean &&
                        mesh->drawRanges.size() == Water::StitchVariantCount;
                    const auto appendRange = [&](const std::uint32_t rangeShaderSlot, const std::uint32_t sectionIndex, const std::uint32_t firstIndex,
                                                 const std::uint32_t indexCount, const std::uint32_t firstMeshlet,
                                                 const std::uint32_t meshletCount, const AABB& rangeBounds,
                                                 const bool usesFoliagePipeline, const AlphaMode alphaMode,
                                                 const bool twoSided, const bool forceDistinctBatch,
                                                 const PBRMaterial& pbrMaterial) {
                    const BatchKey batchKey{renderer.mesh.resource().get(), sectionIndex, rangeShaderSlot, usesFoliagePipeline,
                                            castShadow, renderer.shadowCacheMode, renderer.cullingBatch};
                    const auto [batchIt, inserted] = !forceDistinctBatch && optimizationFeatures.instancedRendering
                        ? batchIndices.try_emplace(batchKey, instanceBatches.size())
                        : std::pair{batchIndices.end(), true};
                    const std::size_t batchIndex = !forceDistinctBatch && optimizationFeatures.instancedRendering
                        ? batchIt->second : instanceBatches.size();
                    AABB displacedRangeBounds = rangeBounds;
                    if (pbrMaterial.displacementTexture >= 0) {
                        const float d0 = pbrMaterial.displacementOffset;
                        const float d1 = pbrMaterial.displacementOffset + pbrMaterial.displacementScale;
                        const float expansion = std::max(std::abs(d0), std::abs(d1));
                        const Vec3 padding{expansion, expansion, expansion};
                        displacedRangeBounds.min -= padding;
                        displacedRangeBounds.max += padding;
                    }
                    const AABB rangeWorldBounds = displacedRangeBounds.transformed(worldModel(entity));
                    if (inserted) {
                        instanceBatches.push_back(InstanceBatch{
                            .mesh = renderer.mesh.resource().get(),
                            .firstIndex = renderer.firstIndex + firstIndex,
                            .indexCount = indexCount,
                            .lod1IndexCount = 0,
                            .lod2IndexCount = 0,
                            .firstMeshlet = (firstMeshlets.contains(renderer.mesh.resource().get())
                                ? firstMeshlets.at(renderer.mesh.resource().get()) : 0U) + firstMeshlet,
                            .meshletCount = meshletCount,
                            .firstMeshletCluster = firstMeshletClusters.contains(renderer.mesh.resource().get())
                                ? firstMeshletClusters.at(renderer.mesh.resource().get()) : 0U,
                            .meshletClusterRoot = firstMeshletClusters.contains(renderer.mesh.resource().get())
                                ? firstMeshletClusters.at(renderer.mesh.resource().get()) +
                                  (sectionIndex < mesh->renderSections.size()
                                      ? mesh->renderSections[sectionIndex].meshletClusterRoot
                                      : mesh->meshletClusterRoot) : 0U,
                            .firstInstance = static_cast<uint32_t>(renderables.size()),
                            .instanceCount = 0,
                            .shaderSlot = rangeShaderSlot,
                            .castShadow = castShadow,
                            .shadowCacheMode = renderer.shadowCacheMode,
                            // The foliage stream is drawn after opaque geometry. Route
                            // blend here until transparent draws have a sorted stream.
                            .twoSided = twoSided,
                            .foliagePipeline = usesFoliagePipeline,
                            .alphaMode = alphaMode,
                            .worldBounds = rangeWorldBounds,
                        });
                        sceneGpu.batchRenderableIndices.emplace_back();
                    }
                    InstanceBatch& batch = instanceBatches[batchIndex];
                    if (batch.instanceCount == 0) {
                        batch.worldBounds = rangeWorldBounds;
                    } else {
                        batch.worldBounds.min = Vec3{
                            std::min(batch.worldBounds.min.x(), rangeWorldBounds.min.x()),
                            std::min(batch.worldBounds.min.y(), rangeWorldBounds.min.y()),
                            std::min(batch.worldBounds.min.z(), rangeWorldBounds.min.z())};
                        batch.worldBounds.max = Vec3{
                            std::max(batch.worldBounds.max.x(), rangeWorldBounds.max.x()),
                            std::max(batch.worldBounds.max.y(), rangeWorldBounds.max.y()),
                            std::max(batch.worldBounds.max.z(), rangeWorldBounds.max.z())};
                    }
                    ++batch.instanceCount;
                    renderables.push_back({.entity = entity, .localBounds = displacedRangeBounds, .batchIndex = batchIndex,
                                           .firstVertex = firstVertex, .vertexCount = mesh->vertexCount(),
                                           .sectionIndex = sectionIndex});
                    const std::size_t renderableIndex = renderables.size() - 1;
                    sceneGpu.batchRenderableIndices[batchIndex].push_back(renderableIndex);
                    sceneGpu.renderableIndices[entity].push_back(renderableIndex);
                    sceneMinimum = glm::min(sceneMinimum, rangeWorldBounds.min.native());
                    sceneMaximum = glm::max(sceneMaximum, rangeWorldBounds.max.native());
                    };
                    if (virtualOcean) {
                        // Virtual-ocean pages are owned by VirtualWaterRenderer. Keep exactly one
                        // zero-index generic record so the shared scene instance/material streams
                        // retain this entity without sending its 448 pages through generic culling.
                        AABB oceanBounds{
                            .min = {-Water::OceanExtents.back(), -2.0F, -Water::OceanExtents.back()},
                            .max = { Water::OceanExtents.back(),  2.0F,  Water::OceanExtents.back()},
                        };
                        appendRange(shaderSlot, 0, 0, 0, 0, 0, oceanBounds, overrideUsesFoliagePipeline,
                            renderer.material.pbr.alphaMode, renderer.material.pbr.doubleSided, false,
                            renderer.material.pbr);
                    } else if (!mesh->renderSections.empty()) {
                        for (std::uint32_t sectionIndex = 0; sectionIndex < mesh->renderSections.size(); ++sectionIndex) {
                            const Mesh::RenderSection& section = mesh->renderSections[sectionIndex];
                            const PBRMaterial& material = section.materialIndex < mesh->materials.size()
                                ? mesh->materials[section.materialIndex] : PBRMaterial{};
                            const PBRMaterial& effectiveMaterial = renderer.materialOverride ? renderer.material.pbr : material;
                            const bool usesFoliagePipeline = renderer.materialOverride ? overrideUsesFoliagePipeline :
                                (material.doubleSided || material.alphaMode == AlphaMode::Mask || material.alphaMode == AlphaMode::Blend);
                            appendRange(pbrShaderSlot(renderer.materialOverride ? renderer.material.pbr : material),
                                sectionIndex, section.firstIndex, section.indexCount, section.firstMeshlet,
                                section.meshletCount, section.localBounds, usesFoliagePipeline,
                                effectiveMaterial.alphaMode, effectiveMaterial.doubleSided, false,
                                effectiveMaterial);
                        }
                    } else {
                        appendRange(pbrShaderSlot(renderer.material.pbr), 0, 0, mesh->indexCount(), 0, static_cast<std::uint32_t>(mesh->meshlets.size()),
                                    localBounds, overrideUsesFoliagePipeline, renderer.material.pbr.alphaMode,
                                    renderer.material.pbr.doubleSided, false, renderer.material.pbr);
                    }
                });

            // Painted grass stays compact in the ECS and is expanded into
            // spatially-local GPU batches.  A single terrain-wide batch has
            // an AABB as large as the whole terrain, so frustum culling can
            // never reject it: every blade is drawn even when most grass is
            // behind the camera.  Small contiguous clusters retain instanced
            // draws while giving the GPU culler useful bounds.
            registry.view<Transform, TerrainGrassComponent>(
                [&](const Entity entity, const Transform& terrainTransform,
                    TerrainGrassComponent& grass) {
                    if (!grass.hasPrefab() || grass.instances.empty()) return;
                    auto& gpuIndices = sceneGpu.grassInstanceGpuIndices[entity];
                    gpuIndices.assign(grass.instances.size(), std::numeric_limits<std::uint32_t>::max());
                    const Mesh* mesh = grass.mesh.get();
                    MeshUploadRecord upload = grassUploads.at(entity);
                    upload.localBounds = {
                        .min = Vec3{std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
                                    std::numeric_limits<float>::max()},
                        .max = Vec3{std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest(),
                                    std::numeric_limits<float>::lowest()},
                    };
                    for (const Vertex& vertex : mesh->vertices) {
                        upload.localBounds.min = Vec3{glm::min(upload.localBounds.min.native(), vertex.position.native())};
                        upload.localBounds.max = Vec3{glm::max(upload.localBounds.max.native(), vertex.position.native())};
                    }

                    // Authoring changes compact legacy exceptions into fixed
                    // terrain chunks once. Rendering no longer rebuilds a
                    // hash table and scans the complete field every frame.
                    grass.rebuildChunks();

                    const float horizontalTerrainScale = std::max(
                        std::abs(terrainTransform.scale.x()), std::abs(terrainTransform.scale.z()));
                    const float meshHeight = upload.localBounds.max.y() - upload.localBounds.min.y();
                    // The packed renderer places vertices at root + yaw *
                    // (localPosition * scale).  A radius about local origin,
                    // rather than the AABB center, remains valid even for a
                    // mesh whose root is not centered in its local bounds.
                    const glm::vec3 maxRootExtent = glm::max(
                        glm::abs(upload.localBounds.min.native()),
                        glm::abs(upload.localBounds.max.native()));
                    const float meshRootRadius = glm::length(maxRootExtent);
                    for (const GrassChunk& chunk : grass.chunks) {
                        AABB batchBounds{};
                        float largestScale = 0.0F;
                        bool firstBounds = true;
                        const std::size_t end = static_cast<std::size_t>(chunk.instanceOffset) +
                                                chunk.instanceCount;
                        for (std::size_t grassIndex = chunk.instanceOffset; grassIndex < end; ++grassIndex) {
                            const auto& item = grass.instances[grassIndex];
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
                            largestScale = std::max(largestScale, item.scale);
                        }
                        // The angle clamp guarantees a maximum horizontal
                        // reach of height * sin(60 degrees). Keep a small
                        // margin for the mesh's authored width.
                        const float bendExpansion = meshHeight * largestScale * horizontalTerrainScale * 0.9F;
                        batchBounds.min.setX(batchBounds.min.x() - bendExpansion);
                        batchBounds.min.setZ(batchBounds.min.z() - bendExpansion);
                        batchBounds.max.setX(batchBounds.max.x() + bendExpansion);
                        batchBounds.max.setZ(batchBounds.max.z() + bendExpansion);
                        // Grass is represented exclusively by packed records;
                        // it must never enter renderables/instanceBatches.
                        const std::uint32_t packedOffset = static_cast<std::uint32_t>(
                            sceneGpu.grassInstances.size());
                        const float extentX = std::max(batchBounds.max.x() - batchBounds.min.x(), 1.0e-4F);
                        const float extentZ = std::max(batchBounds.max.z() - batchBounds.min.z(), 1.0e-4F);
                        const float extent = std::max(extentX, extentZ);
                        const auto packUnorm16 = [](const float value) {
                            return static_cast<std::uint32_t>(std::round(
                                std::clamp(value, 0.0F, 1.0F) * 65535.0F));
                        };
                        const auto packHalf = [](const float value) {
                            return glm::packHalf1x16(value);
                        };
                        for (std::size_t grassIndex = chunk.instanceOffset; grassIndex < end; ++grassIndex) {
                            const auto& item = grass.instances[grassIndex];
                            const Transform local{.position = item.position,
                                                  .rotation = Vec3{0.0F, item.yaw, 0.0F},
                                                  .scale = Vec3{item.scale, item.scale, item.scale}};
                            const glm::mat4 model = terrainTransform.matrix().native() * local.matrix().native();
                            const glm::vec3 worldPosition{model[3]};
                            const std::uint32_t packedX = packUnorm16(
                                (worldPosition.x - batchBounds.min.x()) / extent);
                            const std::uint32_t packedZ = packUnorm16(
                                (worldPosition.z - batchBounds.min.z()) / extent);
                            // Terrain authoring stores yaw in degrees.
                            const std::uint32_t packedYaw = packUnorm16(item.yaw / 360.0F);
                            const std::uint32_t seed = static_cast<std::uint32_t>(grassIndex) & 0xffffU;
                            sceneGpu.grassInstances.push_back({
                                .packedXZ = packedX | (packedZ << 16U),
                                .packedYRotation = packHalf(worldPosition.y) | (packedYaw << 16U),
                                .packedScaleSeed = packHalf(item.scale) | (seed << 16U),
                                // The shader resolves its cluster through
                                // this field; grassType remains in the
                                // cluster record for batching/variants.
                                .flags = static_cast<std::uint32_t>(sceneGpu.grassClusters.size()),
                            });
                            const auto packDeformation = [](const float bendX, const float bendZ,
                                                            const float trampled) {
                                const auto snorm8 = [](const float value) {
                                    return static_cast<std::uint32_t>(static_cast<std::uint8_t>(
                                        std::lround(std::clamp(value, -1.0F, 1.0F) * 127.0F)));
                                };
                                const auto unorm8 = [](const float value) {
                                    return static_cast<std::uint32_t>(std::lround(
                                        std::clamp(value, 0.0F, 1.0F) * 255.0F));
                                };
                                return snorm8(bendX) | (snorm8(bendZ) << 8U) |
                                       (unorm8(trampled) << 16U);
                            };
                            const std::uint32_t deformation = packDeformation(
                                item.bendX, item.bendZ, item.trampled);
                            sceneGpu.grassDeformations.push_back({deformation, deformation});
                            gpuIndices[grassIndex] = static_cast<std::uint32_t>(
                                sceneGpu.grassDeformations.size() - 1);
                        }
                        sceneGpu.grassClusters.push_back({
                            .originExtent = {batchBounds.min.x(), batchBounds.min.z(), extent, meshRootRadius},
                            .instanceRange = {packedOffset, chunk.instanceCount,
                                              0U, grass.grassType},
                            .draw = {upload.firstIndex, mesh->indexCount(), 0U,
                                     glm::packHalf1x16((batchBounds.min.y() + batchBounds.max.y()) * 0.5F) |
                                         (glm::packHalf1x16((batchBounds.max.y() - batchBounds.min.y()) * 0.5F) << 16U)},
                            .bladeShape = {upload.localBounds.min.y(), std::max(meshHeight, 1.0e-4F),
                                           1.0F, glm::radians(60.0F)},
                        });
                        sceneGpu.grassClusterEntities.push_back(entity);
                        sceneMinimum = glm::min(sceneMinimum, batchBounds.min.native());
                        sceneMaximum = glm::max(sceneMaximum, batchBounds.max.native());
                    }
                });

            // An editor scene is allowed to be empty.  Render passes still
            // bind vertex/index/instance/material buffers even when there are
            // no draw calls, so keep one harmless dummy element in each GPU
            // buffer instead of failing scene synchronization after deleting
            // the final mesh object.
            if (vertexCount == 0 || indexCount == 0) {
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
                // A mesh-free water scene still needs its renderer-owned
                // transform slots uploaded for VirtualWaterRenderer.
                if (!renderables.empty()) createInstanceBuffer();
                [[maybe_unused]] const UploadTicket ticket = uploadBatch.submit();
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

            // Compute generation samples the same normalized height data used
            // by TerrainComponent::sampleHeight.  The first terrain is the
            // active grass domain; scenes with several terrains receive one
            // grass component/renderer domain per future extension.
            grassHeightTexture.destroy();
            grassDensityTexture.destroy();
            registry.view<TerrainComponent>([&](const Entity, const TerrainComponent& terrain) {
                if (grassHeightTexture.valid() || !terrain.valid()) return;
                std::vector<std::uint8_t> encodedHeights(terrain.sampleCount());
                const float range = std::max(terrain.maximumHeight - terrain.minimumHeight, 1.0e-6F);
                for (std::size_t i = 0; i < encodedHeights.size(); ++i) {
                    const float normalized = std::clamp((terrain.heights[i] - terrain.minimumHeight) / range,
                                                        0.0F, 1.0F);
                    encodedHeights[i] = static_cast<std::uint8_t>(std::round(normalized * 255.0F));
                }
                grassHeightTexture.create(vulkanDevice.physical(), device, commandPool,
                    vulkanDevice.graphicsQueue(), terrain.resolution, terrain.resolution, encodedHeights,
                    TextureColorSpace::Linear, false, vulkanDevice.allocator(), TexturePixelFormat::R8);
                // Preserve existing terrain scenes: until the editor exposes a
                // paintable density layer, every valid terrain texel is a
                // candidate and the compute shader's chunk density controls
                // coverage. Roads/water can later write zero into this map.
                std::vector<std::uint8_t> density(terrain.sampleCount(), 255U);
                grassDensityTexture.create(vulkanDevice.physical(), device, commandPool,
                    vulkanDevice.graphicsQueue(), terrain.resolution, terrain.resolution, density,
                    TextureColorSpace::Linear, false, vulkanDevice.allocator(), TexturePixelFormat::R8);
            });
            rayTracingBlasDirty = vulkanDevice.supportsRayQuery();
            [[maybe_unused]] const UploadTicket ticket = uploadBatch.submit();
        }

        void createInstanceBuffer() {
            // Resource creation can happen before the first render frame, so
            // establish the same transform stage used by the frame pipeline.
            TransformSystem::updateDirty(registry);
            instanceModels.resize(renderables.size());
            // updateRenderableBuffers() initializes the current transform
            // stream and preserves the prior pose for each changed record.
            previousInstanceTransforms.resize(renderables.size());
            // All sections of an entity share its material table. Geometry is
            // sectioned for culling, not duplicated material ownership.
            std::uint32_t nextMaterialOffset = 0;
            std::unordered_map<Entity, std::uint32_t> renderableMaterialOffsets;
            for (RenderableRecord& record : renderables) {
                const auto [it, inserted] = renderableMaterialOffsets.try_emplace(record.entity, nextMaterialOffset);
                if (inserted) nextMaterialOffset += materialSlots;
                record.materialTableOffset = it->second;
            }
            std::unordered_map<Entity, std::uint32_t> grassMaterialOffsets;
            registry.view<TerrainGrassComponent>([&](const Entity entity, const TerrainGrassComponent& grass) {
                if (!grass.hasPrefab() || grass.instances.empty()) return;
                grassMaterialOffsets.emplace(entity, nextMaterialOffset);
                nextMaterialOffset += materialSlots;
            });
            materials.resize(nextMaterialOffset);
            // Clusters inherit the same shared material table as their
            // owning TerrainGrassComponent. The grass shader consumes this
            // offset directly instead of assuming material zero.
            for (std::size_t cluster = 0; cluster < sceneGpu.grassClusters.size(); ++cluster) {
                const Entity owner = sceneGpu.grassClusterEntities[cluster];
                const auto material = grassMaterialOffsets.find(owner);
                if (material == grassMaterialOffsets.end()) { continue;
}
                sceneGpu.grassClusters[cluster].instanceRange.z = material->second;
            }

            // updateRenderableBuffers() performs incremental writes for the
            // current frame.  A topology rebuild may have changed both the
            // number of records and the global material-table stride, so
            // reserve every target before it can issue any such write.
            const auto ensureHostVisibleCapacity = [&](auto& buffers, const std::size_t requiredCount,
                                                        const VkDeviceSize elementSize,
                                                        const VkBufferUsageFlags usage) {
                const std::size_t minimumCount = std::max<std::size_t>(1, requiredCount);
                for (Buffer& buffer : buffers) {
                    const std::size_t oldCapacity = static_cast<std::size_t>(buffer.size() / elementSize);
                    const std::size_t capacity = std::max(minimumCount, oldCapacity + oldCapacity / 2U);
                    if (buffer.handle() == VK_NULL_HANDLE || oldCapacity < minimumCount) {
                        buffer.createHostVisible(vulkanDevice.physical(), device, capacity * elementSize,
                            usage, vulkanDevice.allocator());
                    }
                }
            };
            ensureHostVisibleCapacity(instanceBuffers, instanceModels.size(), sizeof(RendererInstanceData),
                VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
            if (antialiasingLevel == AntialiasingLevel::TAA) {
                const std::size_t minimumCount = std::max<std::size_t>(1, previousInstanceTransforms.size());
                for (std::size_t frame = 0; frame < previousTransformBuffers.size(); ++frame) {
                    Buffer& current = previousTransformBuffers[frame];
                    const std::size_t oldCapacity = static_cast<std::size_t>(
                        current.size() / sizeof(RendererPreviousTransformData));
                    if (current.handle() == VK_NULL_HANDLE || oldCapacity < minimumCount) {
                        Buffer replacement;
                        const std::size_t capacity = std::max(minimumCount, oldCapacity + oldCapacity / 2U);
                        replacement.createHostVisible(vulkanDevice.physical(), device,
                            capacity * sizeof(RendererPreviousTransformData),
                            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, vulkanDevice.allocator());
                        if (current.handle() != VK_NULL_HANDLE) {
                            deferredPreviousTransformBuffers[frame].emplace_back(std::move(current));
                        }
                        current = std::move(replacement);
                    }
                }
            }
            ensureHostVisibleCapacity(materialBuffers, materials.size(), sizeof(GPUMaterialData),
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
            updateRenderableBuffers();
            // Packed grass has no RenderableRecord. Populate its shared
            // material ranges directly from TerrainGrassComponent instead.
            registry.view<TerrainGrassComponent>([&](const Entity entity, const TerrainGrassComponent& grass) {
                const auto offsetIt = grassMaterialOffsets.find(entity);
                if (!grass.hasPrefab() || offsetIt == grassMaterialOffsets.end()) return;
                const Mesh& mesh = *grass.mesh;
                for (std::uint32_t slot = 0; slot < materialSlots; ++slot) {
                    const PBRMaterial source = mesh.materials.empty() ? grass.material :
                        (slot < mesh.materials.size() ? mesh.materials[slot] : PBRMaterial{});
                    materials[offsetIt->second + slot] = packMaterial(source, mesh);
                }
            });
            for (std::size_t index = 0; index < instanceModels.size(); ++index) {
                const RendererInstanceData& model = instanceModels[index];
                previousInstanceTransforms[index] = {
                    .previousPosition = glm::vec4{glm::vec3{model.positionMaterial}, 0.0F},
                    .previousRotation = model.rotation,
                    .previousScale = model.scaleBase,
                };
            }
            for (Buffer& buffer : instanceBuffers) {
                const VkDeviceSize required = sizeof(RendererInstanceData) *
                    std::max<std::size_t>(1, instanceModels.size());
                if (buffer.handle() == VK_NULL_HANDLE || buffer.size() < required) {
                    buffer.createHostVisible(vulkanDevice.physical(), device, required + required / 2U,
                        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                        vulkanDevice.allocator());
                }
                if (!instanceModels.empty()) {
                    buffer.update(instanceModels.data(),
                                  sizeof(RendererInstanceData) * instanceModels.size());
                }
            }
            if (antialiasingLevel == AntialiasingLevel::TAA) {
                for (Buffer& buffer : previousTransformBuffers) {
                    const VkDeviceSize required = sizeof(RendererPreviousTransformData) *
                        std::max<std::size_t>(1, previousInstanceTransforms.size());
                    if (buffer.handle() == VK_NULL_HANDLE || buffer.size() < required) {
                        buffer.createHostVisible(vulkanDevice.physical(), device, required + required / 2U,
                            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, vulkanDevice.allocator());
                    }
                    if (!previousInstanceTransforms.empty()) {
                        buffer.update(previousInstanceTransforms.data(),
                            sizeof(RendererPreviousTransformData) * previousInstanceTransforms.size());
                    }
                }
            } else {
                for (std::size_t frame = 0; frame < previousTransformBuffers.size(); ++frame) {
                    Buffer& current = previousTransformBuffers[frame];
                    if (current.handle() != VK_NULL_HANDLE) {
                        deferredPreviousTransformBuffers[frame].emplace_back(std::move(current));
                    }
                }
            }
            // Binding 6 is part of the forward/shadow descriptor contract and
            // therefore must exist before createShadowPass(). The first half
            // is an identity map for ordinary draws; compute appends compact
            // grass indices in the second half.
            const std::size_t instanceCount = std::max<std::size_t>(1, instanceModels.size());
            std::vector<std::uint32_t> instanceIndexMap(instanceCount * 2U, 0U);
            for (std::uint32_t index = 0; index < instanceCount; ++index) instanceIndexMap[index] = index;
            const VkDeviceSize compactGrassInstancesRequired =
                sizeof(std::uint32_t) * instanceIndexMap.size();
            constexpr VkBufferUsageFlags compactGrassInstancesUsage =
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
            for (Buffer& buffer : compactGrassInstanceBuffers) {
                if (buffer.handle() == VK_NULL_HANDLE) {
                    buffer.createDeviceLocal(vulkanDevice.physical(), device,
                        instanceIndexMap.data(), compactGrassInstancesRequired,
                        compactGrassInstancesUsage, commandPool,
                        vulkanDevice.graphicsQueue(), vulkanDevice.allocator());
                } else if (buffer.size() >= compactGrassInstancesRequired) {
                    buffer.uploadDeviceLocal(instanceIndexMap.data(),
                        compactGrassInstancesRequired, 0, commandPool,
                        vulkanDevice.graphicsQueue());
                } else {
                    // Preserve the old allocation until its upload has
                    // completed; Buffer move-assignment retires it safely.
                    Buffer replacement;
                    const VkDeviceSize capacity = compactGrassInstancesRequired +
                        compactGrassInstancesRequired / 2U;
                    replacement.createDeviceLocalEmpty(device, capacity,
                        compactGrassInstancesUsage, vulkanDevice.allocator());
                    replacement.uploadDeviceLocal(instanceIndexMap.data(),
                        compactGrassInstancesRequired, 0, commandPool,
                        vulkanDevice.graphicsQueue());
                    buffer = std::move(replacement);
                }
            }
            for (Buffer& buffer : materialBuffers) {
                const VkDeviceSize required = sizeof(GPUMaterialData) *
                    std::max<std::size_t>(1, materials.size());
                if (buffer.handle() == VK_NULL_HANDLE || buffer.size() < required) {
                    buffer.createHostVisible(vulkanDevice.physical(), device, required + required / 2U,
                        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, vulkanDevice.allocator());
                }
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

            createGPUSceneDatabaseBuffers();
            // All renderer uploads have consumed the decoded payload. A
            // MeshCollider owns a separate source reference until PhysX has
            // cooked it; procedural/editor meshes are explicitly pinned.
            releaseUploadedMeshSourceData();
        }

        [[nodiscard]] static GPUSceneInstanceRecord gpuSceneRecord(
            const GPUSceneDatabase::GPUInstance& instance) {
            glm::mat4 worldMatrix{1.0F};
            for (glm::length_t column = 0; column < 4; ++column) {
                for (glm::length_t row = 0; row < 4; ++row) {
                    worldMatrix[column][row] = instance.worldMatrix[column * 4 + row];
                }
            }
            // A full snapshot includes retained, dead slots so their stable
            // IDs can be reused safely. Preserve the same GPU-visible inert
            // representation used by the incremental removal upload; otherwise
            // a rebuild would resurrect a slot whose stored flags still have
            // the visibility bit set.
            const std::uint32_t flags = instance.alive ? instance.flags : 0U;
            return {
                .worldMatrix = worldMatrix,
                .localBoundsMin = glm::vec4{instance.localBounds.min.native(), 0.0F},
                .localBoundsMax = glm::vec4{instance.localBounds.max.native(), 0.0F},
                .idsAndFlags = glm::uvec4{instance.meshId, instance.materialId,
                                          instance.objectId, flags},
            };
        }

        [[nodiscard]] static GPUVisibilityInstanceRecord gpuSceneVisibilityRecord(
            const GPUSceneDatabase::GPUInstance& instance) {
            glm::mat4 worldMatrix{1.0F};
            for (glm::length_t column = 0; column < 4; ++column) {
                for (glm::length_t row = 0; row < 4; ++row) {
                    worldMatrix[column][row] = instance.worldMatrix[column * 4 + row];
                }
            }

            const glm::vec3 localCenter = (instance.localBounds.min.native() +
                                           instance.localBounds.max.native()) * 0.5F;
            const glm::vec3 localExtent = (instance.localBounds.max.native() -
                                           instance.localBounds.min.native()) * 0.5F;
            const glm::vec3 worldCenter = glm::vec3{worldMatrix * glm::vec4{localCenter, 1.0F}};
            // Transform local extent axes separately. The length of the
            // resulting conservative world AABB extent encloses it in a
            // sphere, which remains valid under rotation and non-uniform scale.
            const glm::vec3 worldExtent =
                glm::abs(glm::vec3{worldMatrix[0]}) * localExtent.x +
                glm::abs(glm::vec3{worldMatrix[1]}) * localExtent.y +
                glm::abs(glm::vec3{worldMatrix[2]}) * localExtent.z;
            const std::uint32_t flags = instance.alive ? instance.flags : 0U;
            return {
                .worldCenterRadius = glm::vec4{worldCenter, glm::length(worldExtent)},
                .idsAndFlags = glm::uvec4{instance.meshId, instance.materialId,
                                          instance.objectId, flags},
            };
        }

        [[nodiscard]] static GPUSceneMeshRecord gpuSceneRecord(
            const GPUSceneDatabase::GPUMesh& mesh) {
            return {
                .draw = glm::uvec4{mesh.firstIndex, mesh.indexCount,
                                   static_cast<std::uint32_t>(mesh.vertexOffset),
                                   mesh.lod1IndexCount},
                .lod = glm::uvec4{mesh.lod2IndexCount, mesh.firstMeshlet,
                                  mesh.meshletCount, 0U},
                // z is the hierarchy-present flag. The meshlet culler uses
                // it to select the descendant-range traversal; the node
                // ranges themselves provide the exact bounds for each step.
                .clusters = glm::uvec4{mesh.firstMeshletCluster, mesh.meshletClusterRoot,
                                       mesh.meshletCount != 0U ? 1U : 0U, 0U},
            };
        }

        [[nodiscard]] static GPUSceneMaterialRecord gpuSceneRecord(
            const GPUSceneDatabase::GPUMaterial& material) {
            return {.data = glm::uvec4{material.materialTableOffset,
                                       material.pipelineClass, material.flags, 0U}};
        }

        template <typename GPURecord, typename SourceRecord>
        [[nodiscard]] static std::vector<GPURecord> makeGPUSceneSnapshot(
            const std::vector<SourceRecord>& source) {
            std::vector<GPURecord> snapshot;
            snapshot.reserve(source.size());
            for (const SourceRecord& record : source) {
                if constexpr (std::is_same_v<GPURecord, GPUVisibilityInstanceRecord>)
                    snapshot.push_back(gpuSceneVisibilityRecord(record));
                else
                    snapshot.push_back(gpuSceneRecord(record));
            }
            return snapshot;
        }

        template <typename GPURecord, typename SourceRecord>
        void uploadGPUSceneSnapshot(Buffer& buffer, const std::vector<SourceRecord>& source) {
            if (source.empty()) return;
            const std::vector<GPURecord> snapshot = makeGPUSceneSnapshot<GPURecord>(source);
            buffer.uploadDeviceLocal(snapshot.data(), sizeof(GPURecord) * snapshot.size(), 0,
                commandPool, vulkanDevice.graphicsQueue());
        }

        template <typename Id>
        static void appendPendingIds(std::vector<Id>& destination, std::vector<std::uint32_t>& stamps,
                                     const std::uint32_t generation, const std::vector<Id>& source) {
            for (const Id id : source) {
                if (id >= stamps.size()) stamps.resize(static_cast<std::size_t>(id) + 1U);
                if (stamps[id] == generation) continue;
                stamps[id] = generation;
                destination.push_back(id);
            }
        }

        void collectGPUSceneDatabaseChanges() {
            const GPUSceneDatabase::DirtyRanges& dirty = sceneGpu.database.dirty();
            if (dirty.instances.empty() && dirty.meshes.empty() && dirty.materials.empty() &&
                dirty.removedInstances.empty()) { return;
}
            for (auto& pending : sceneGpu.pendingDatabaseUploads) {
                appendPendingIds(pending.instances, pending.instanceStamps, pending.generation, dirty.instances);
                appendPendingIds(pending.meshes, pending.meshStamps, pending.generation, dirty.meshes);
                appendPendingIds(pending.materials, pending.materialStamps, pending.generation, dirty.materials);
                appendPendingIds(pending.removedInstances, pending.removedInstanceStamps, pending.generation,
                                 dirty.removedInstances);
            }
            sceneGpu.database.clearDirty();
        }

        void createGPUSceneDatabaseBuffers() {
            auto uploadBatch = uploadContext.beginBatch();
            const auto& instances = sceneGpu.database.instances();
            const auto& meshes = sceneGpu.database.meshes();
            const auto& databaseMaterials = sceneGpu.database.materials();
            gpuSceneInstanceHighWater = std::max(gpuSceneInstanceHighWater, instances.size());
            gpuSceneMeshHighWater = std::max(gpuSceneMeshHighWater, meshes.size());
            gpuSceneMaterialHighWater = std::max(gpuSceneMaterialHighWater, databaseMaterials.size());
            for (std::uint32_t frame = 0; frame < MAX_FRAMES_IN_FLIGHT; ++frame) {
                const auto ensureCapacity = [&](Buffer& buffer, const std::size_t count,
                                                const std::size_t highWater, const std::size_t minimum,
                                                const VkDeviceSize recordSize) {
                    const std::size_t capacity = std::max({minimum, count + count / 2U,
                                                           highWater + highWater / 2U});
                    const VkDeviceSize required = recordSize * capacity;
                    if (buffer.handle() == VK_NULL_HANDLE || buffer.size() < required) {
                        buffer.createDeviceLocalEmpty(device, required,
                            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, vulkanDevice.allocator(), true);
                    }
                };
                ensureCapacity(gpuSceneInstanceBuffers[frame], instances.size(), gpuSceneInstanceHighWater,
                    4096U, sizeof(GPUSceneInstanceRecord));
                ensureCapacity(gpuVisibilityInstanceBuffers[frame], instances.size(), gpuSceneInstanceHighWater,
                    4096U, sizeof(GPUVisibilityInstanceRecord));
                ensureCapacity(gpuSceneMeshBuffers[frame], meshes.size(), gpuSceneMeshHighWater,
                    1024U, sizeof(GPUSceneMeshRecord));
                ensureCapacity(gpuSceneMaterialBuffers[frame], databaseMaterials.size(), gpuSceneMaterialHighWater,
                    1024U, sizeof(GPUSceneMaterialRecord));
                uploadGPUSceneSnapshot<GPUSceneInstanceRecord>(gpuSceneInstanceBuffers[frame], instances);
                uploadGPUSceneSnapshot<GPUVisibilityInstanceRecord>(gpuVisibilityInstanceBuffers[frame], instances);
                uploadGPUSceneSnapshot<GPUSceneMeshRecord>(gpuSceneMeshBuffers[frame], meshes);
                uploadGPUSceneSnapshot<GPUSceneMaterialRecord>(gpuSceneMaterialBuffers[frame], databaseMaterials);
                refreshGPUSceneRoot(frame);
                sceneGpu.pendingDatabaseUploads[frame].clear();
            }
            sceneGpu.database.clearDirty();
            recordGPUSceneUploadBarrier();
            [[maybe_unused]] const UploadTicket ticket = uploadBatch.submit();
        }

        void recordGPUSceneUploadBarrier() {
            if (!uploadContext.recording()) return;
            const VkMemoryBarrier2 barrier{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                .srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT, .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
                .dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT};
            const VkDependencyInfo dependency{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                .memoryBarrierCount = 1, .pMemoryBarriers = &barrier};
            vkCmdPipelineBarrier2(uploadContext.graphicsCommandBuffer(), &dependency);
        }

        void refreshGPUSceneRoot(const std::uint32_t frame) {
            gpuSceneRoots[frame] = {
                .objects = {gpuSceneInstanceBuffers[frame].deviceAddress()},
                .materials = {gpuSceneMaterialBuffers[frame].deviceAddress()},
                .meshes = {gpuSceneMeshBuffers[frame].deviceAddress()},
                .objectCount = static_cast<std::uint32_t>(sceneGpu.database.instances().size()),
                .materialCount = static_cast<std::uint32_t>(sceneGpu.database.materials().size()),
                .meshCount = static_cast<std::uint32_t>(sceneGpu.database.meshes().size()),
            };
        }

        // A frame owns its own GPU-scene snapshot.  drawFrame() only reaches
        // this after waiting for that frame's fence, so replacing this frame's
        // mapped buffers and rewriting its descriptors cannot invalidate work
        // which is still executing on the GPU for another frame.
        [[nodiscard]] bool ensureGPUSceneDatabaseCapacity(const std::uint32_t frame) {
            const auto requiredBytes = [](const std::size_t count, const VkDeviceSize recordSize) {
                return recordSize * std::max<std::size_t>(1, count);
            };
            const auto grow = [&](Buffer& buffer, const VkDeviceSize required) {
                if (buffer.handle() != VK_NULL_HANDLE && buffer.size() >= required) return false;

                // Keep the allocation amortized, but never allocate less than
                // the complete snapshot that is about to be uploaded.
                const VkDeviceSize capacity = buffer.handle() == VK_NULL_HANDLE
                    ? required + required / 2U
                    : std::max(required, buffer.size() * 2U);
                buffer.createDeviceLocalEmpty(device, capacity,
                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, vulkanDevice.allocator(), true);
                return true;
            };

            const bool instanceResized = grow(gpuSceneInstanceBuffers[frame], requiredBytes(
                sceneGpu.database.instances().size(), sizeof(GPUSceneInstanceRecord)));
            const bool visibilityResized = grow(gpuVisibilityInstanceBuffers[frame], requiredBytes(
                sceneGpu.database.instances().size(), sizeof(GPUVisibilityInstanceRecord)));
            const bool meshResized = grow(gpuSceneMeshBuffers[frame], requiredBytes(
                sceneGpu.database.meshes().size(), sizeof(GPUSceneMeshRecord)));
            const bool materialResized = grow(gpuSceneMaterialBuffers[frame], requiredBytes(
                sceneGpu.database.materials().size(), sizeof(GPUSceneMaterialRecord)));
            gpuSceneInstanceHighWater = std::max(gpuSceneInstanceHighWater, sceneGpu.database.instances().size());
            gpuSceneMeshHighWater = std::max(gpuSceneMeshHighWater, sceneGpu.database.meshes().size());
            gpuSceneMaterialHighWater = std::max(gpuSceneMaterialHighWater, sceneGpu.database.materials().size());
            refreshGPUSceneRoot(frame);
            return instanceResized || visibilityResized || meshResized || materialResized;
        }

        void refreshGPUSceneDescriptors(const std::uint32_t frame) const {
            // These are the only descriptors that directly retain GPU-scene
            // buffers.  VkBuffer handles change when a mapped buffer grows.
            if (cullingDescriptorPool == VK_NULL_HANDLE ||
                instanceCullSets[frame] == VK_NULL_HANDLE) return;

            const VkDescriptorBufferInfo instanceInfo{
                gpuVisibilityInstanceBuffers[frame].handle(), 0, VK_WHOLE_SIZE};
            const VkDescriptorBufferInfo visibleInfo{
                visibleInstanceBuffers[frame].handle(), 0, VK_WHOLE_SIZE};
            const VkDescriptorBufferInfo visibleCountInfo{
                visibleInstanceCountBuffers[frame].handle(), 0, VK_WHOLE_SIZE};
            const VkDescriptorBufferInfo cullingUniformInfo{
                cullingUniformBuffers[frame].handle(), 0, sizeof(Culling::CullingUniformData)};
            const std::array<const VkDescriptorBufferInfo*, 4> instanceInfos{
                &instanceInfo, &visibleInfo, &visibleCountInfo, &cullingUniformInfo};
            std::array<VkWriteDescriptorSet, 4> instanceWrites{};
            for (std::uint32_t binding = 0; binding < instanceWrites.size(); ++binding) {
                instanceWrites[binding] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                    .dstSet = instanceCullSets[frame], .dstBinding = binding, .descriptorCount = 1,
                    .descriptorType = binding == 3 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
                                                   : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                    .pBufferInfo = instanceInfos[binding]};
            }
            vkUpdateDescriptorSets(device, static_cast<std::uint32_t>(instanceWrites.size()),
                                   instanceWrites.data(), 0, nullptr);

            if (meshletCullSets[frame] != VK_NULL_HANDLE && meshletBuffer.handle() != VK_NULL_HANDLE) {
                const VkDescriptorBufferInfo meshletInfos[] = {
                    {meshletBuffer.handle(), 0, VK_WHOLE_SIZE},
                    {gpuSceneInstanceBuffers[frame].handle(), 0, VK_WHOLE_SIZE},
                    {gpuSceneMeshBuffers[frame].handle(), 0, VK_WHOLE_SIZE},
                    {visibleInstanceBuffers[frame].handle(), 0, VK_WHOLE_SIZE},
                    {visibleInstanceCountBuffers[frame].handle(), 0, sizeof(std::uint32_t)},
                    {visibleMeshletBuffers[frame].handle(), 0, VK_WHOLE_SIZE},
                    {visibleMeshletCountBuffers[frame].handle(), 0, sizeof(std::uint32_t)},
                    {meshletCullingUniformBuffers[frame].handle(), 0, sizeof(Culling::MeshletCullUniforms)},
                    {meshletClusterBuffer.handle(), 0, VK_WHOLE_SIZE},
                };
                const auto& hiZBuffer = hiZBuffers[frame];
                const VkDescriptorImageInfo meshletHiZInfo{hiZBuffer.sampler(), hiZBuffer.fullView(),
                                                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
                VkWriteDescriptorSet meshletWrites[10]{};
                for (std::uint32_t binding = 0; binding < std::size(meshletWrites); ++binding) {
                    meshletWrites[binding] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                        .dstSet = meshletCullSets[frame], .dstBinding = binding, .descriptorCount = 1,
                        .descriptorType = binding == 8 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER :
                                          (binding == 7 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER),
                        .pImageInfo = binding == 8 ? &meshletHiZInfo : nullptr,
                        .pBufferInfo = binding == 8 ? nullptr : &meshletInfos[binding]};
                }
                vkUpdateDescriptorSets(device, std::size(meshletWrites), meshletWrites, 0, nullptr);
            }
            if (meshletDispatchSets[frame] != VK_NULL_HANDLE) {
                const VkDescriptorBufferInfo meshletDispatchInfos[] = {
                    {visibleInstanceCountBuffers[frame].handle(), 0, sizeof(std::uint32_t)},
                    {meshletCullDispatchBuffers[frame].handle(), 0, sizeof(VkDispatchIndirectCommand)},
                };
                VkWriteDescriptorSet meshletDispatchWrites[2]{};
                for (std::uint32_t binding = 0; binding < std::size(meshletDispatchWrites); ++binding) {
                    meshletDispatchWrites[binding] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                        .dstSet = meshletDispatchSets[frame], .dstBinding = binding, .descriptorCount = 1,
                        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                        .pBufferInfo = &meshletDispatchInfos[binding]};
                }
                vkUpdateDescriptorSets(device, std::size(meshletDispatchWrites),
                                       meshletDispatchWrites, 0, nullptr);
            }
            if (meshletIndirectSets[frame] != VK_NULL_HANDLE) {
                const VkDescriptorBufferInfo meshletIndirectInfos[] = {
                    {visibleMeshletBuffers[frame].handle(), 0, VK_WHOLE_SIZE},
                    {visibleMeshletCountBuffers[frame].handle(), 0, sizeof(std::uint32_t)},
                    {meshletTaskIndirectBuffers[frame].handle(), 0,
                     sizeof(VkDrawMeshTasksIndirectCommandEXT)},
                    {meshletTaskDrawCountBuffers[frame].handle(), 0, sizeof(std::uint32_t)},
                };
                VkWriteDescriptorSet meshletIndirectWrites[4]{};
                for (std::uint32_t binding = 0; binding < std::size(meshletIndirectWrites); ++binding) {
                    meshletIndirectWrites[binding] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                        .dstSet = meshletIndirectSets[frame], .dstBinding = binding,
                        .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                        .pBufferInfo = &meshletIndirectInfos[binding]};
                }
                vkUpdateDescriptorSets(device, std::size(meshletIndirectWrites),
                                       meshletIndirectWrites, 0, nullptr);
            }

            const auto updateGrassSet = [&](const VkDescriptorSet set,
                                            std::initializer_list<VkBuffer> storageBuffers,
                                            const VkBuffer uniformBuffer) {
                if (set == VK_NULL_HANDLE) return;
                std::vector<VkDescriptorBufferInfo> infos;
                infos.reserve(storageBuffers.size() + 1U);
                for (const VkBuffer buffer : storageBuffers) infos.push_back({buffer, 0, VK_WHOLE_SIZE});
                infos.push_back({uniformBuffer, 0, VK_WHOLE_SIZE});
                std::vector<VkWriteDescriptorSet> writes(infos.size());
                for (std::uint32_t binding = 0; binding < writes.size(); ++binding) {
                    writes[binding] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                        .dstSet = set, .dstBinding = binding, .descriptorCount = 1,
                        .descriptorType = binding + 1 == writes.size() ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
                                                                         : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                        .pBufferInfo = &infos[binding]};
                }
                vkUpdateDescriptorSets(device, static_cast<std::uint32_t>(writes.size()),
                                       writes.data(), 0, nullptr);
            };
            updateGrassSet(grassBuildSets[frame], {gpuSceneInstanceBuffers[frame].handle(),
                visibleInstanceBuffers[frame].handle(), visibleInstanceCountBuffers[frame].handle(),
                gpuSceneMeshBuffers[frame].handle(), grassBinCountBuffers[frame].handle()},
                grassIndirectUniformBuffers[frame].handle());
            updateGrassSet(grassScatterSets[frame], {gpuSceneInstanceBuffers[frame].handle(),
                visibleInstanceBuffers[frame].handle(), visibleInstanceCountBuffers[frame].handle(),
                grassBinOffsetBuffers[frame].handle(), grassBinCursorBuffers[frame].handle(),
                compactGrassInstanceBuffers[frame].handle()}, grassIndirectUniformBuffers[frame].handle());
            updateGrassSet(grassFinalizeSets[frame], {gpuSceneMeshBuffers[frame].handle(),
                grassBinCountBuffers[frame].handle(), grassBinOffsetBuffers[frame].handle(),
                grassIndirectBuffers[frame].handle(), grassDrawCountBuffers[frame].handle()},
                grassIndirectUniformBuffers[frame].handle());
        }

        void uploadPendingGPUSceneDatabase(const std::uint32_t frame) {
            collectGPUSceneDatabaseChanges();
            auto& pending = sceneGpu.pendingDatabaseUploads[frame];
            if (pending.instances.empty() && pending.meshes.empty() && pending.materials.empty() &&
                pending.removedInstances.empty()) return;
            auto uploadBatch = uploadContext.beginBatch();
            if (ensureGPUSceneDatabaseCapacity(frame)) {
                // A replacement allocation has no old contents.  Upload the
                // complete per-frame snapshot and repoint descriptors before
                // any compute or draw command can consume it.
                const auto& instances = sceneGpu.database.instances();
                const auto& meshes = sceneGpu.database.meshes();
                const auto& databaseMaterials = sceneGpu.database.materials();
                uploadGPUSceneSnapshot<GPUSceneInstanceRecord>(gpuSceneInstanceBuffers[frame], instances);
                uploadGPUSceneSnapshot<GPUVisibilityInstanceRecord>(gpuVisibilityInstanceBuffers[frame], instances);
                uploadGPUSceneSnapshot<GPUSceneMeshRecord>(gpuSceneMeshBuffers[frame], meshes);
                uploadGPUSceneSnapshot<GPUSceneMaterialRecord>(gpuSceneMaterialBuffers[frame], databaseMaterials);
                refreshGPUSceneDescriptors(frame);
                pending.clear();
                recordGPUSceneUploadBarrier();
                [[maybe_unused]] const UploadTicket ticket = uploadBatch.submit();
                return;
            }
            const auto& instances = sceneGpu.database.instances();
            for (const GPUSceneInstanceId id : pending.instances) {
                if (id >= instances.size()) continue;
                const auto record = gpuSceneRecord(instances[id]);
                gpuSceneInstanceBuffers[frame].uploadDeviceLocal(&record, sizeof(record), sizeof(record) * id,
                    commandPool, vulkanDevice.graphicsQueue());
                const auto visibilityRecord = gpuSceneVisibilityRecord(instances[id]);
                gpuVisibilityInstanceBuffers[frame].uploadDeviceLocal(
                    &visibilityRecord, sizeof(visibilityRecord), sizeof(visibilityRecord) * id,
                    commandPool, vulkanDevice.graphicsQueue());
            }
            // A removed slot retains its fixed address but becomes inert. This
            // makes an in-flight indirect list harmless even before compaction.
            for (const GPUSceneInstanceId id : pending.removedInstances) {
                if (id >= instances.size()) continue;
                auto record = gpuSceneRecord(instances[id]);
                record.idsAndFlags.w = 0U;
                gpuSceneInstanceBuffers[frame].uploadDeviceLocal(&record, sizeof(record), sizeof(record) * id,
                    commandPool, vulkanDevice.graphicsQueue());
                auto visibilityRecord = gpuSceneVisibilityRecord(instances[id]);
                visibilityRecord.idsAndFlags.w = 0U;
                gpuVisibilityInstanceBuffers[frame].uploadDeviceLocal(
                    &visibilityRecord, sizeof(visibilityRecord), sizeof(visibilityRecord) * id,
                    commandPool, vulkanDevice.graphicsQueue());
            }
            const auto& meshes = sceneGpu.database.meshes();
            for (const GPUSceneMeshId id : pending.meshes) {
                if (id >= meshes.size()) continue;
                const auto record = gpuSceneRecord(meshes[id]);
                gpuSceneMeshBuffers[frame].uploadDeviceLocal(&record, sizeof(record), sizeof(record) * id,
                    commandPool, vulkanDevice.graphicsQueue());
            }
            const auto& databaseMaterials = sceneGpu.database.materials();
            for (const GPUSceneMaterialId id : pending.materials) {
                if (id >= databaseMaterials.size()) continue;
                const auto record = gpuSceneRecord(databaseMaterials[id]);
                gpuSceneMaterialBuffers[frame].uploadDeviceLocal(&record, sizeof(record), sizeof(record) * id,
                    commandPool, vulkanDevice.graphicsQueue());
            }
            pending.clear();
            recordGPUSceneUploadBarrier();
            [[maybe_unused]] const UploadTicket ticket = uploadBatch.submit();
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

            if (!sceneGpu.grassDeformations.empty() &&
                uploadedGrassDeformationVersions[currentFrame] != grassDeformationVersion) {
                grassDeformationBuffers[currentFrame].update(sceneGpu.grassDeformations.data(),
                    sizeof(GPUGrassDeformation) * sceneGpu.grassDeformations.size());
                uploadedGrassDeformationVersions[currentFrame] = grassDeformationVersion;
            }

            const uint8_t bit = frameBit(currentFrame);
            // The dirty list is populated for every frame-in-flight. Sort it
            // here so adjacent instance IDs become a single mapped-buffer
            // write; static instances remain entirely untouched.
            std::ranges::sort(dirtyTransforms[currentFrame]);
            uploadDirtyIndices<RendererInstanceData>({
                .buffer = instanceBuffers[currentFrame],
                .data = instanceModels,
                .dirtyFrames = &RenderableRecord::transformDirtyFrames,
                .indices = dirtyTransforms[currentFrame],
            });
            if (antialiasingLevel == AntialiasingLevel::TAA) {
                uploadDirtyIndices<RendererPreviousTransformData>({
                    .buffer = previousTransformBuffers[currentFrame],
                    .data = previousInstanceTransforms,
                    .dirtyFrames = &RenderableRecord::transformDirtyFrames,
                    .indices = dirtyTransforms[currentFrame],
                });
            }
            // Once this frame has consumed the old/current pair, future
            // frames must see a stationary pair unless the transform changes
            // again. Keep just the M uploaded IDs while their dirty bit is
            // advanced to the next frame-in-flight.
            const std::vector<std::size_t> uploadedTransforms = dirtyTransforms[currentFrame];
            for (const std::size_t index : uploadedTransforms) {
                const RendererInstanceData& rendererInstance = instanceModels[index];
                previousInstanceTransforms[index] = {
                    .previousPosition = glm::vec4{glm::vec3{rendererInstance.positionMaterial}, 0.0F},
                    .previousRotation = rendererInstance.rotation,
                    .previousScale = rendererInstance.scaleBase,
                };
            }
            clearDirtyIndices(&RenderableRecord::transformDirtyFrames,
                              dirtyTransforms[currentFrame], bit);
            for (const std::size_t index : uploadedTransforms) {
                markDirty(index, &RenderableRecord::transformDirtyFrames, dirtyTransforms);
            }
            for (const std::size_t index : dirtyMaterials[currentFrame                                                                        ]) {
                const RenderableRecord& record = renderables[index];
                materialBuffers[currentFrame].update(
                    materials.data() + record.materialTableOffset,
                    sizeof(GPUMaterialData) * materialSlots,
                    sizeof(GPUMaterialData) * record.materialTableOffset);
            }
            clearDirtyIndices(&RenderableRecord::materialDirtyFrames,
                              dirtyMaterials[currentFrame], bit);
            uploadPendingGPUSceneDatabase(currentFrame);
        }

        void updateRenderableBuffers() {
            const auto modelFromInstance = [](const RendererInstanceData& instance) {
                const glm::quat rotation{instance.rotation.w, instance.rotation.x,
                                         instance.rotation.y, instance.rotation.z,};
                glm::mat4 model = glm::translate(glm::mat4{1.0F},
                                                 glm::vec3{instance.positionMaterial});
                model *= glm::mat4_cast(rotation);
                return glm::scale(model, glm::vec3{instance.scaleBase});
            };
            dirtyShadowObjects.clear();
            const bool shadowCacheCanContainGeometry =
                lastTransformRevision != std::numeric_limits<std::uint64_t>::max() &&
                lastMeshRendererRevision != std::numeric_limits<std::uint64_t>::max() &&
                lastTerrainGrassRevision != std::numeric_limits<std::uint64_t>::max() &&
                lastParentRevision != std::numeric_limits<std::uint64_t>::max();
            const auto appendDirtyShadowBounds = [&](const AABB& bounds) {
                if (!shadowCacheCanContainGeometry) { return;
}
                Culling::GPUObjectData object{};
                object.localAabbMin = {bounds.min.x(), bounds.min.y(), bounds.min.z(), 0.0F};
                object.localAabbMax = {bounds.max.x(), bounds.max.y(), bounds.max.z(), 0.0F};
                object.model.data[0] = 1.0F;
                object.model.data[5] = 1.0F;
                object.model.data[10] = 1.0F;
                object.model.data[15] = 1.0F;
                dirtyShadowObjects.push_back(object);
            };
            const std::uint64_t transformRevision = registry.componentRevision<Transform>();
            const std::uint64_t meshRendererRevision = registry.componentRevision<MeshRenderer>();
            const std::uint64_t terrainGrassRevision = registry.componentRevision<TerrainGrassComponent>();
            const std::uint64_t parentRevision = registry.componentRevision<ParentComponent>();
            if (terrainGrassRevision != lastTerrainGrassRevision) {
                const auto packDeformation = [](const TerrainGrassInstance& item) {
                    const auto snorm8 = [](const float value) {
                        return static_cast<std::uint32_t>(static_cast<std::uint8_t>(
                            std::lround(std::clamp(value, -1.0F, 1.0F) * 127.0F)));
                    };
                    const auto unorm8 = [](const float value) {
                        return static_cast<std::uint32_t>(std::lround(
                            std::clamp(value, 0.0F, 1.0F) * 255.0F));
                    };
                    return snorm8(item.bendX) | (snorm8(item.bendZ) << 8U) |
                           (unorm8(item.trampled) << 16U);
                };
                bool deformationChanged = false;
                registry.view<TerrainGrassComponent>([&](const Entity entity,
                                                         const TerrainGrassComponent& grass) {
                    const auto found = sceneGpu.grassInstanceGpuIndices.find(entity);
                    if (found == sceneGpu.grassInstanceGpuIndices.end()) return;
                    const auto updateOne = [&](const std::size_t sourceIndex) {
                        if (sourceIndex >= found->second.size() || sourceIndex >= grass.instances.size()) return;
                        const std::uint32_t gpuIndex = found->second[sourceIndex];
                        if (gpuIndex >= sceneGpu.grassDeformations.size()) return;
                        GPUGrassDeformation& destination = sceneGpu.grassDeformations[gpuIndex];
                        const std::uint32_t current = packDeformation(grass.instances[sourceIndex]);
                        if (destination.packedCurrent == current) return;
                        destination.packedPrevious = destination.packedCurrent;
                        destination.packedCurrent = current;
                        deformationChanged = true;
                    };
                    if (grass.allInstancesDirty) {
                        for (std::size_t index = 0; index < grass.instances.size(); ++index) updateOne(index);
                    } else {
                        for (const std::size_t index : grass.dirtyInstances) updateOne(index);
                    }
                });
                if (deformationChanged) ++grassDeformationVersion;
            }
            if (transformRevision == lastTransformRevision &&
                meshRendererRevision == lastMeshRendererRevision &&
                terrainGrassRevision == lastTerrainGrassRevision &&
                parentRevision == lastParentRevision) {
                uploadPendingRenderableBuffers();
                return;
            }

            std::vector<std::size_t> changedIndices;
            changedIndices.reserve(renderables.size());
            constexpr std::uint8_t transformChange = 1U;
            constexpr std::uint8_t rendererChange = 2U;
            if (renderableChangeMarks.size() != renderables.size()) {
                renderableChangeMarks.assign(renderables.size(), 0);
                renderableChangeKinds.assign(renderables.size(), 0);
                renderableChangeEpoch = 0;
            }
            if (lastTransformRevision == std::numeric_limits<std::uint64_t>::max() ||
                lastMeshRendererRevision == std::numeric_limits<std::uint64_t>::max()) {
                for (std::size_t index = 0; index < renderables.size(); ++index) {
                    changedIndices.push_back(index);
                }
            } else {
                ++renderableChangeEpoch;
                if (renderableChangeEpoch == 0) {
                    std::fill(renderableChangeMarks.begin(), renderableChangeMarks.end(), 0);
                    renderableChangeEpoch = 1;
                }
                const auto addIndex = [&](const std::size_t index, const std::uint8_t kind) {
                    if (renderableChangeMarks[index] != renderableChangeEpoch) {
                        renderableChangeMarks[index] = renderableChangeEpoch;
                        renderableChangeKinds[index] = kind;
                        changedIndices.push_back(index);
                    } else {
                        renderableChangeKinds[index] |= kind;
                    }
                };
                const auto addChangedEntities = [&](const auto& entities, const auto revision,
                                                    const std::uint8_t kind) {
                    if (revision == 0) { return;
}

                    for (const Entity entity : entities) {
                        const auto it = sceneGpu.renderableIndices.find(entity);
                        if (it != sceneGpu.renderableIndices.end())
                            for (const std::size_t index : it->second) addIndex(index, kind);
                    }
                };
                addChangedEntities(
                    TransformSystem::changedWorldTransforms(registry), transformRevision, transformChange);
                addChangedEntities(
                    registry.componentEntitiesChangedSince<MeshRenderer>(lastMeshRendererRevision),
                    meshRendererRevision, rendererChange);
            }

            const Registry& readRegistry = registry;
            const auto worldModel = [&](const Entity entity) {
                return readRegistry.get<Transform>(entity).worldMatrix().native();
            };
            std::vector<std::size_t> changedBatches;
            changedBatches.reserve(changedIndices.size());
            for (const std::size_t index : changedIndices) {
                const Entity entity = renderables[index].entity;
                RenderableRecord& record = renderables[index];
                if (!readRegistry.has<Transform>(entity)) {
                    sceneGpu.database.removeInstance(static_cast<std::uint64_t>(entity),
                                                     submittedFrameValue);
                    record.renderProxy.instance = InvalidGPUSceneInstanceId;
                    continue;
                }
                const auto& transform = readRegistry.get<Transform>(entity);
                if (record.waterOnly) {
                    const bool transformChanged = lastTransformRevision ==
                        std::numeric_limits<std::uint64_t>::max() ||
                        (!optimizationFeatures.transformCaching ||
                         record.lastWorldRevision != transform.worldRevision());
                    if (transformChanged) {
                        const glm::mat4 model = worldModel(entity);
                        glm::vec3 scale{};
                        glm::quat rotation{};
                        glm::vec3 translation{};
                        glm::vec3 skew{};
                        glm::vec4 perspective{};
                        if (!glm::decompose(model, scale, rotation, translation, skew, perspective)) {
                            scale = {1.0F, 1.0F, 1.0F};
                            rotation = {};
                            translation = glm::vec3{model[3]};
                        }
                        RendererInstanceData& instance = instanceModels[index];
                        previousInstanceTransforms[index] = {
                            .previousPosition = glm::vec4{glm::vec3{instance.positionMaterial}, 0.0F},
                            .previousRotation = instance.rotation,
                            .previousScale = instance.scaleBase,
                        };
                        instance.positionMaterial = glm::vec4{translation, 0.0F};
                        instance.rotation = {rotation.x, rotation.y, rotation.z, rotation.w};
                        instance.scaleBase = {scale, 0.0F};
                        record.lastWorldRevision = transform.worldRevision();
                        markDirty(index, &RenderableRecord::transformDirtyFrames, dirtyTransforms);
                    }
                    continue;
                }
                if (!readRegistry.has<MeshRenderer>(entity)) {
                    sceneGpu.database.removeInstance(static_cast<std::uint64_t>(entity),
                                                     submittedFrameValue);
                    record.renderProxy.instance = InvalidGPUSceneInstanceId;
                    continue;
                }
                const bool hasTransformChange = lastTransformRevision ==
                    std::numeric_limits<std::uint64_t>::max() ||
                    renderableChangeKinds[index] & transformChange;
                const bool hasRendererChange = lastMeshRendererRevision ==
                    std::numeric_limits<std::uint64_t>::max() ||
                    renderableChangeKinds[index] & rendererChange;
                const MeshRenderer* renderer = &readRegistry.get<MeshRenderer>(entity);
                glm::mat4 model = worldModel(entity);
                const auto shadowBounds = [&](const glm::mat4& instanceModel) {
                    return record.localBounds.transformed(instanceModel);
                };
                const bool transformChanged = hasTransformChange &&
                    (!optimizationFeatures.transformCaching ||
                     record.lastWorldRevision != transform.worldRevision());
                if (transformChanged) {
                    // Preserve history only for an instance whose pose is
                    // changing. The dirty upload below carries both poses to
                    // every frame-in-flight, so velocity always observes the
                    // immediately preceding transform without an O(N) pass.
                    RendererPreviousTransformData& previousTransform = previousInstanceTransforms[index];
                    RendererInstanceData& rendererInstance = instanceModels[index];
                    previousTransform = {
                        .previousPosition = glm::vec4{glm::vec3{rendererInstance.positionMaterial}, 0.0F},
                        .previousRotation = rendererInstance.rotation,
                        .previousScale = rendererInstance.scaleBase,
                    };
                    const bool hadWorldTransform = record.lastWorldRevision !=
                        std::numeric_limits<std::uint64_t>::max();
                    const AABB previousShadowBounds = hadWorldTransform
                        ? shadowBounds(modelFromInstance(instanceModels[index])) : AABB{};
                    glm::vec3 decomposedScale{};
                    glm::quat decomposedRotation{};
                    glm::vec3 decomposedTranslation{};
                    glm::vec3 skew{};
                    glm::vec4 perspective{};
                    if (!glm::decompose(model, decomposedScale, decomposedRotation,
                                        decomposedTranslation, skew, perspective)) {
                        decomposedScale = {1.0F, 1.0F, 1.0F};
                        decomposedRotation = {};
                        decomposedTranslation = glm::vec3{model[3]};
                    }
                    rendererInstance.positionMaterial = glm::vec4{
                        decomposedTranslation, std::bit_cast<float>(record.materialTableOffset)};
                    rendererInstance.rotation = glm::vec4{decomposedRotation.x, decomposedRotation.y,
                                                           decomposedRotation.z, decomposedRotation.w};
                    rendererInstance.scaleBase = glm::vec4{decomposedScale, 0.0F};
                    record.lastWorldRevision = transform.worldRevision();
                    if (record.batchIndex < instanceBatches.size() &&
                        instanceBatches[record.batchIndex].castShadow) {
                        if (hadWorldTransform) appendDirtyShadowBounds(previousShadowBounds);
                        appendDirtyShadowBounds(shadowBounds(model));
                    }
                    markDirty(index, &RenderableRecord::transformDirtyFrames, dirtyTransforms);
                    markDirty(index, &RenderableRecord::cullingDirtyFrames, dirtyCullingObjects);
                    changedBatches.push_back(record.batchIndex);
                }
                bool materialChanged = false;
                if (hasRendererChange) {
                    const auto sourceMesh = renderer->mesh.source();
                    if (sourceMesh) {
                        const Mesh& mesh = *sourceMesh;
                        for (std::uint32_t slot = 0; slot < materialSlots; ++slot) {
                        const PBRMaterial source = mesh.materials.empty() ||
                            (renderer->materialOverride && slot == 0)
                            ? renderer->material.pbr
                            : (slot < mesh.materials.size() ? mesh.materials[slot] : PBRMaterial{});
                        const WaterMaterial* const water = renderer->materialOverride && slot == 0 &&
                            renderer->material.shaderSource == MaterialShaderSource::BuiltIn &&
                            renderer->material.shader == MaterialShader::Water
                            ? &renderer->material.water : nullptr;
                        const GPUMaterialData material = packMaterial(source, mesh, water);
                        GPUMaterialData& destination = materials[record.materialTableOffset + slot];
                        if (!optimizationFeatures.materialCaching ||
                            !sameMaterial(destination, material)) {
                            destination = material;
                            materialChanged = true;
                        }
                        }
                    }
                }
                if (materialChanged) {
                    if (record.batchIndex < instanceBatches.size() &&
                        instanceBatches[record.batchIndex].castShadow) {
                        appendDirtyShadowBounds(shadowBounds(model));
                    }
                    markDirty(index, &RenderableRecord::materialDirtyFrames, dirtyMaterials);
                }

                {
                    const InstanceBatch& batch = instanceBatches[record.batchIndex];
                    const GPUSceneDatabase::GPUMesh mesh{
                        .firstIndex = batch.firstIndex,
                        .indexCount = batch.indexCount,
                        .vertexOffset = 0,
                        .lod1IndexCount = batch.lod1IndexCount,
                        .lod2IndexCount = batch.lod2IndexCount,
                        .firstMeshlet = batch.firstMeshlet,
                        .meshletCount = batch.meshletCount,
                        .firstMeshletCluster = batch.firstMeshletCluster,
                        .meshletClusterRoot = batch.meshletClusterRoot,
                    };
                    const GPUSceneDatabase::GPUMaterial material{
                        .materialTableOffset = record.materialTableOffset,
                        .pipelineClass = batch.foliagePipeline ? 1U : 0U,
                        .flags = batch.castShadow ? 1U : 0U,
                    };
                    const auto worldMatrix = [&model] {
                        std::array<float, 16> matrix{};
                        for (glm::length_t column = 0; column < 4; ++column) {
                            for (glm::length_t row = 0; row < 4; ++row) {
                                matrix[column * 4 + row] = model[column][row];
                            }
                        }
                        return matrix;
                    }();
                    const std::uint32_t instanceFlags = 1U | (batch.foliagePipeline ? 2U : 0U);
                    const bool proxyUninitialized = record.renderProxy.instance == InvalidGPUSceneInstanceId;
                    if (proxyUninitialized) {
                        const std::uint64_t meshKey = static_cast<std::uint64_t>(
                            reinterpret_cast<std::uintptr_t>(batch.mesh)) ^
                            (static_cast<std::uint64_t>(record.sectionIndex) << 32U);
                        record.renderProxy.mesh = sceneGpu.database.upsertMesh(meshKey, mesh);
                        record.renderProxy.material = sceneGpu.database.upsertMaterial(
                            (static_cast<std::uint64_t>(record.materialTableOffset) << 32U) |
                            static_cast<std::uint64_t>(record.sectionIndex), material);
                        const std::uint64_t instanceKey = (static_cast<std::uint64_t>(entity) << 32U) |
                            static_cast<std::uint64_t>(index);
                        record.renderProxy.instance = sceneGpu.database.upsertInstance(instanceKey, {
                                .worldMatrix = worldMatrix,
                                .localBounds = record.localBounds,
                                .meshId = record.renderProxy.mesh,
                                .materialId = record.renderProxy.material,
                                .objectId = static_cast<std::uint32_t>(index),
                                .flags = instanceFlags,
                            });
                    } else {
                        // A transform-only update deliberately does no hashing,
                        // lookup or write to the mesh/material tables.
                        if (transformChanged) {
                            sceneGpu.database.updateInstanceTransform(
                                record.renderProxy.instance, worldMatrix, record.localBounds);
                        }
                        if (hasRendererChange) {
                            sceneGpu.database.updateMesh(record.renderProxy.mesh, mesh);
                            sceneGpu.database.updateMaterial(record.renderProxy.material, material);
                            sceneGpu.database.updateInstanceFlags(record.renderProxy.instance, instanceFlags);
                        }
                    }
                }
            }
            if (gpuObjects.size() == instanceBatches.size() && !changedBatches.empty()) {
                std::ranges::sort(changedBatches);
                changedBatches.erase(std::ranges::unique(changedBatches).begin(), changedBatches.end());
                for (const std::size_t batchIndex : changedBatches) {
                    if (batchIndex >= sceneGpu.batchRenderableIndices.size()) continue;
                    AABB bounds{};
                    bool initialized = false;
                    for (std::size_t index : sceneGpu.batchRenderableIndices[batchIndex]) {
                        const RenderableRecord& record = renderables[index];
                        if (!readRegistry.has<Transform>(record.entity)) continue;
                        const glm::mat4 instanceModel = modelFromInstance(instanceModels[index]);
                        const AABB worldBounds = record.localBounds.transformed(instanceModel);
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
                grass.clearDirtyInstances();
                grass.allInstancesDirty = false;
            });
            lastTransformRevision = transformRevision;
            lastMeshRendererRevision = meshRendererRevision;
            lastTerrainGrassRevision = terrainGrassRevision;
            lastParentRevision = parentRevision;
        }

        [[nodiscard]] bool canUseHiZOcclusionCulling() const noexcept {
            constexpr std::size_t minimumHiZRenderableCount = 256;
            return optimizationFeatures.gpuCulling && optimizationFeatures.occlusionCulling &&
                   instanceBatches.size() >= minimumHiZRenderableCount &&
                   (!msaa.enabled() || vulkanDevice.supportsConservativeDepthResolve());
        }

        void updateCullingUniformBuffer(const uint32_t frame) const {
            // Empty editor scenes intentionally do not allocate culling
            // resources. The render passes already skip zero-object draws,
            // so there is no uniform buffer to update in that case.
            if (cullingUniformBuffers[frame].handle() == VK_NULL_HANDLE) return;
            constexpr float hizDepthBias = 0.0025F;
            constexpr float hizAabbExpansion = 0.01F;
            const auto& hiZBuffer = hiZBuffers[frame];
            Culling::CullingUniformData data{};
            if (!cameraController.camera()) {
                throw std::runtime_error("Camera must be initialized before culling");
            }
            const glm::mat4 viewProjection = cameraController.camera()->projectionMatrix().native() * cameraController.camera()->viewMatrix().native();
            std::memcpy(data.viewProjection.data, &viewProjection, sizeof(viewProjection));
            std::memcpy(data.occlusionViewProjection.data, &hiZViewProjections[frame],
                        sizeof(hiZViewProjections[frame]));
            const auto frustumPlanes = extractFrustumPlanes(viewProjection);
            for (std::size_t i = 0; i < frustumPlanes.size(); ++i)
                std::memcpy(&data.frustumPlanes[i], &frustumPlanes[i], sizeof(frustumPlanes[i]));
            data.cameraPosition = {cameraController.camera()->position().x(), cameraController.camera()->position().y(),
                                   cameraController.camera()->position().z(), 1.0F};
            data.objectCount = static_cast<uint32_t>(gpuObjects.size());
            data.maxDrawCount = data.objectCount;
            data.hizMipCount = hiZBuffer.mipCount();
            data.enableOcclusionCulling = canUseHiZOcclusionCulling() ? 1U : 0U;
            data.viewportWidth = static_cast<float>(swapchain.extent().width);
            data.viewportHeight = static_cast<float>(swapchain.extent().height);
            data.depthBias = hizDepthBias;
            data.aabbExpansion = hizAabbExpansion;
            // Never reject objects using an uninitialized hierarchy.
            data.cameraCut = hiZValid[frame] ? 0u : 1u;
            data.shadowPass = 0;
            data.enableFrustumCulling = optimizationFeatures.gpuCulling ? 1u : 0u;
            data.drawCategory = 0;
            cullingUniformBuffers[frame].update(&data, sizeof(data));
            data.drawCategory = 1;
            foliageCullingUniformBuffers[frame].update(&data, sizeof(data));
        }

        void updateMeshletCullingUniformBuffer(const uint32_t frame) const {
            if (meshletCullingUniformBuffers[frame].handle() == VK_NULL_HANDLE ||
                !cameraController.camera()) return;
            Culling::MeshletCullUniforms data{};
            const glm::mat4 viewProjection = cameraController.camera()->projectionMatrix().native() *
                                             cameraController.camera()->viewMatrix().native();
            std::memcpy(data.viewProjection.data, &viewProjection, sizeof(viewProjection));
            std::memcpy(data.occlusionViewProjection.data, &hiZViewProjections[frame],
                        sizeof(hiZViewProjections[frame]));
            const auto planes = extractFrustumPlanes(viewProjection);
            for (std::size_t index = 0; index < planes.size(); ++index)
                std::memcpy(&data.frustumPlanes[index], &planes[index], sizeof(planes[index]));
            data.cameraX = cameraController.camera()->position().x();
            data.cameraY = cameraController.camera()->position().y();
            data.cameraZ = cameraController.camera()->position().z();
            data.meshletCount = globalMeshletCount;
            data.viewportWidth = static_cast<float>(swapchain.extent().width);
            data.viewportHeight = static_cast<float>(swapchain.extent().height);
            data.depthBias = 0.0025F;
            data.hiZMipCount = hiZValid[frame] ? hiZBuffers[frame].mipCount() : 0U;
            data.enableOcclusionCulling = canUseHiZOcclusionCulling() ? 1U : 0U;
            data.cameraCut = hiZValid[frame] ? 0U : 1U;
            meshletCullingUniformBuffers[frame].update(&data, sizeof(data));
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
            std::memcpy(data.occlusionViewProjection.data, &viewProjection, sizeof(viewProjection));
            const auto frustumPlanes = extractFrustumPlanes(viewProjection);
            for (std::size_t i = 0; i < frustumPlanes.size(); ++i)
                std::memcpy(&data.frustumPlanes[i], &frustumPlanes[i], sizeof(frustumPlanes[i]));
            data.cameraPosition = {sceneCamera.position().x(), sceneCamera.position().y(), sceneCamera.position().z(), 1.0F};
            data.objectCount = static_cast<uint32_t>(gpuObjects.size());
            data.maxDrawCount = data.objectCount;
            // Virtual Water uses these dimensions to convert a page's NDC
            // bounds to pixels. Leaving them zero rejected every Scene View
            // page as sub-pixel, even though frustum culling accepted it.
            data.viewportWidth = static_cast<float>(sceneViewportTarget.extent().width);
            data.viewportHeight = static_cast<float>(sceneViewportTarget.extent().height);
            data.hizMipCount = 0;
            data.enableOcclusionCulling = 0;
            data.enableFrustumCulling = optimizationFeatures.gpuCulling ? 1U : 0U;
            data.cameraCut = 1;
            data.shadowPass = 0;
            data.drawCategory = 0;
            sceneCullingUniformBuffers[frame].update(&data, sizeof(data));
            data.drawCategory = 1;
            sceneFoliageCullingUniformBuffers[frame].update(&data, sizeof(data));
        }

        void updateShadowCullingUniformBuffer(const uint32_t frame) const {
            if (shadowCullingUniformBuffers[frame].handle() == VK_NULL_HANDLE) return;
            constexpr float hizAabbExpansion = 0.01F;
            Culling::CullingUniformData data{};
            const glm::mat4 lightViewProjection = lightSpaceMatrix().native();
            std::memcpy(data.viewProjection.data, &lightViewProjection, sizeof(lightViewProjection));
            std::memcpy(data.occlusionViewProjection.data, &lightViewProjection, sizeof(lightViewProjection));
            const auto frustumPlanes = extractFrustumPlanes(lightViewProjection);
            for (std::size_t i = 0; i < frustumPlanes.size(); ++i)
                std::memcpy(&data.frustumPlanes[i], &frustumPlanes[i], sizeof(frustumPlanes[i]));
            data.cameraPosition = {0.0F, 0.0F, 0.0F, 1.0F};
            data.objectCount = static_cast<uint32_t>(gpuObjects.size());
            data.maxDrawCount = data.objectCount;
            // Shadow culling uses only the light frustum. Camera Hi-Z cannot safely
            // reject casters which are invisible to the camera but visible to the light.
            data.enableOcclusionCulling = 0;
            data.aabbExpansion = hizAabbExpansion;
            data.cameraCut = 1;
            data.shadowPass = 1;
            data.enableFrustumCulling = optimizationFeatures.gpuCulling ? 1u : 0u;
            data.drawCategory = 0;
            data.maxShadowInstances = static_cast<uint32_t>(std::max<std::size_t>(1, instanceModels.size()));
            shadowCullingUniformBuffers[frame].update(&data, sizeof(data));
            data.drawCategory = 1;
            shadowTwoSidedCullingUniformBuffers[frame].update(&data, sizeof(data));
        }

        void createUniformBuffers() {
            for (Buffer& buffer : uniformBuffers) {
                buffer.createHostVisible(vulkanDevice.physical(), device, sizeof(UniformBufferObject),
                                         VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, vulkanDevice.allocator());
            }
        }

        void createSceneUniformBuffers() {
            // Scene descriptor sets bind ViewRenderScratchResources::uniformBuffers.
            // The storage is created once by createUniformBuffers().
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
