#include "Engine/Scene/SceneSerializer.h"

#include "Engine/Assets/AssetManager.h"
#include "Engine/Core/Transform.h"
#include "Engine/ECS/Components/CameraComponent.h"
#include "Engine/ECS/Components/ColliderComponent.h"
#include "Engine/ECS/Components/RigidbodyComponent.h"
#include "Engine/ECS/Components/ScriptComponent.h"
#include "Engine/ECS/Components/ColorPickerComponent.h"
#include "Engine/ECS/Components/ParticleEmitterComponent.h"
#include "Engine/ECS/Components/SmokeEmitterComponent.h"
#include "Engine/ECS/Components/ProceduralCloudComponent.h"
#include "Engine/ECS/Components/TerrainComponent.h"
#include "Engine/ECS/Components/TerrainGrassComponent.h"
#include "Engine/ECS/Components/WindComponent.h"
#include "Engine/ECS/Registry.h"
#include "Engine/Renderer/MeshRenderer.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/Components/LightComponent.h"
#include "Engine/Scene/Components/IdentityComponents.h"

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <fstream>
#include <limits>
#include <locale>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// NOLINTBEGIN(readability-magic-numbers)

namespace Engine {
    namespace {
        constexpr std::size_t MaxMeshes = 1'000'000;
        constexpr std::size_t MaxVertices = 50'000'000;
        constexpr std::size_t MaxIndices = 150'000'000;
        constexpr std::size_t MaxEntities = 10'000'000;
        constexpr std::size_t FloatBufferSize = 32;
        constexpr std::uint32_t LegacyFormatVersion = 11;
        constexpr std::uint32_t TerrainDataVersion = 1;
        constexpr std::array<char, 8> TerrainDataMagic{'G', 'E', 'T', 'E', 'R', 'R', '1', '\0'};

        [[nodiscard]] int terrainDataStreamSlot() {
            static const int slot = std::ios_base::xalloc();
            return slot;
        }

        [[noreturn]] void invalidScene(const std::string &message) {
            throw std::runtime_error("Invalid scene: " + message);
        }

        [[nodiscard]] std::filesystem::path terrainDataPath(const std::filesystem::path& scenePath) {
            return std::filesystem::path{scenePath.string() + ".terrain"};
        }

        void writeLittleEndianU32(std::ostream& output, const std::uint32_t value) {
            const std::array<char, 4> bytes{
                static_cast<char>(value & 0xFFU), static_cast<char>((value >> 8U) & 0xFFU),
                static_cast<char>((value >> 16U) & 0xFFU), static_cast<char>((value >> 24U) & 0xFFU)};
            output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        }

        [[nodiscard]] std::uint32_t readLittleEndianU32(std::istream& input, const std::string_view description) {
            std::array<unsigned char, 4> bytes{};
            input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            if (!input) invalidScene("could not read " + std::string(description));
            return static_cast<std::uint32_t>(bytes[0]) |
                   (static_cast<std::uint32_t>(bytes[1]) << 8U) |
                   (static_cast<std::uint32_t>(bytes[2]) << 16U) |
                   (static_cast<std::uint32_t>(bytes[3]) << 24U);
        }

        void writeLittleEndianU64(std::ostream& output, const std::uint64_t value) {
            for (unsigned shift = 0; shift < 64; shift += 8) output.put(static_cast<char>((value >> shift) & 0xFFU));
        }

        [[nodiscard]] std::uint64_t readLittleEndianU64(std::istream& input, const std::string_view description) {
            std::uint64_t value = 0;
            for (unsigned shift = 0; shift < 64; shift += 8) {
                const int byte = input.get();
                if (byte == EOF) invalidScene("could not read " + std::string(description));
                value |= static_cast<std::uint64_t>(static_cast<unsigned char>(byte)) << shift;
            }
            return value;
        }

        [[nodiscard]] std::uint64_t terrainChecksum(const TerrainComponent& terrain) noexcept {
            std::uint64_t hash = 1469598103934665603ULL;
            const auto append = [&hash](const float value) {
                const auto bits = std::bit_cast<std::uint32_t>(value);
                for (unsigned shift = 0; shift < 32; shift += 8) {
                    hash ^= (bits >> shift) & 0xFFU;
                    hash *= 1099511628211ULL;
                }
            };
            for (const float value : terrain.heights) append(value);
            for (const Vec3& color : terrain.colors) { append(color.x()); append(color.y()); append(color.z()); }
            return hash;
        }

        [[nodiscard]] std::uint64_t byteChecksum(const std::vector<std::uint8_t>& bytes) noexcept {
            std::uint64_t hash = 1469598103934665603ULL;
            for (const std::uint8_t byte : bytes) {
                hash ^= byte;
                hash *= 1099511628211ULL;
            }
            return hash;
        }

        void expect(std::istream &input, const std::string_view expected) {
            std::string token;
            input >> token;
            if (input.fail() || token != expected) {
                invalidScene("expected '" + std::string(expected) + "'");
            }
        }

        template<typename T>
        T read(std::istream &input, const std::string_view description) {
            T value{};
            input >> value;
            if (input.fail()) {
                invalidScene("could not read " + std::string(description));
            }
            return value;
        }

        float readFloat(std::istream &input, const std::string_view description) {
            const auto value = read<float>(input, description);
            if (!std::isfinite(value)) {
                invalidScene(std::string(description) + " must be finite");
            }
            return value;
        }

        std::size_t readCount(std::istream &input, const std::string_view description,
                              const std::size_t maximum) {
            const auto value = read<unsigned long long>(input, description);
            if (value > maximum) {
                invalidScene(std::string(description) + " exceeds the supported limit");
            }
            return static_cast<std::size_t>(value);
        }

        bool readBool(std::istream &input, const std::string_view description) {
            const int value = read<int>(input, description);
            if (value != 0 && value != 1) {
                invalidScene(std::string(description) + " must be 0 or 1");
            }
            return value != 0;
        }

        Vec3 readVec3(std::istream &input, const std::string_view description) {
            return {
                readFloat(input, description),
                readFloat(input, description),
                readFloat(input, description),
            };
        }

        Math::Color readColor(std::istream &input, const std::string_view description) {
            const float red = readFloat(input, description);
            const float green = readFloat(input, description);
            const float blue = readFloat(input, description);
            return Math::Color::from_rgb(red, green, blue);
        }

        Math::Color readColorRgba(std::istream &input, const std::string_view description) {
            return {
                readFloat(input, std::string(description) + " red"),
                readFloat(input, std::string(description) + " green"),
                readFloat(input, std::string(description) + " blue"),
                readFloat(input, std::string(description) + " alpha")
            };
        }

        void writeFloat(std::ostream &output, const float value) {
            char buffer[FloatBufferSize];
            const auto result = std::to_chars(std::begin(buffer), std::end(buffer), value);
            if (result.ec != std::errc{}) {
                throw std::runtime_error("Could not format scene float");
            }
            output.write(buffer, result.ptr - buffer);
        }

        void writeVec3(std::ostream &output, const Vec3 &value) {
            writeFloat(output, value.x());
            output << ' ';
            writeFloat(output, value.y());
            output << ' ';
            writeFloat(output, value.z());
        }

        void writeColor(std::ostream &output, const Math::Color &value) {
            writeFloat(output, value.r());
            output << ' ';
            writeFloat(output, value.g());
            output << ' ';
            writeFloat(output, value.b());
        }

        void writeColorRgba(std::ostream &output, const Math::Color &value) {
            writeFloat(output, value.r());
            output << ' ';
            writeFloat(output, value.g());
            output << ' ';
            writeFloat(output, value.b());
            output << ' ';
            writeFloat(output, value.a());
        }

        void writeMaterial(std::ostream &output, const PBRMaterial &material) {
            writeColorRgba(output, material.baseColor);
            output << ' ';
            writeFloat(output, material.metallic);
            output << ' ';
            writeFloat(output, material.roughness);
            output << ' ';
            writeFloat(output, material.ambientOcclusion);
            output << ' ' << material.baseColorTexture << ' '
                    << material.metallicRoughnessTexture << ' ' << material.normalTexture << ' ';
            writeFloat(output, material.normalScale);
            output << ' ' << static_cast<int>(material.alphaBlend) << ' '
                    << static_cast<int>(material.doubleSided) << ' ';
            writeFloat(output, material.alphaCutoff);
        }

        PBRMaterial readMaterial(std::istream &input) {
            PBRMaterial material;
            material.baseColor = readColorRgba(input, "material base color");
            material.metallic = readFloat(input, "material metallic");
            material.roughness = readFloat(input, "material roughness");
            material.ambientOcclusion = readFloat(input, "material ambient occlusion");
            material.baseColorTexture = read<std::int32_t>(input, "base-color texture index");
            material.metallicRoughnessTexture = read<std::int32_t>(input, "metallic-roughness texture index");
            material.normalTexture = read<std::int32_t>(input, "normal texture index");
            material.normalScale = readFloat(input, "normal scale");
            material.alphaBlend = readBool(input, "alpha-blend flag");
            material.doubleSided = readBool(input, "double-sided flag");
            material.alphaCutoff = readFloat(input, "alpha cutoff");
            return material;
        }

        void writeParticleEmitter(std::ostream &output, const Particles::ParticleEmitter &emitter) {
            writeVec3(output, emitter.position);
            output << ' ';
            writeVec3(output, emitter.minVelocity);
            output << ' ';
            writeVec3(output, emitter.maxVelocity);
            output << ' ';
            writeColorRgba(output, emitter.color);
            output << ' ';
            writeFloat(output, emitter.minLifeTime);
            output << ' ';
            writeFloat(output, emitter.maxLifeTime);
            output << ' ';
            writeFloat(output, emitter.minSize);
            output << ' ';
            writeFloat(output, emitter.maxSize);
            output << ' ';
            writeFloat(output, emitter.spawnRate);
        }

        void writeSmokeEmitter(std::ostream &output, const Particles::SmokeEmitter &emitter) {
            writeParticleEmitter(output, emitter);
            output << ' ';
            writeFloat(output, emitter.buoyancy);
            output << ' ';
            writeFloat(output, emitter.drag);
            output << ' ';
            writeFloat(output, emitter.turbulence);
            output << ' ';
            writeFloat(output, emitter.collisionRadius);
        }

        void writeCollider(std::ostream &output, const ColliderComponent &collider) {
            output << static_cast<int>(collider.shape.index()) << ' ';
            writeVec3(output, collider.offset);
            output << ' ' << static_cast<int>(collider.isTrigger) << ' ';
            writeFloat(output, collider.friction);
            output << ' ';
            writeFloat(output, collider.restitution);
            std::visit([&output]<typename T>(const T &shape) {
                using Shape = std::decay_t<T>;
                if constexpr (std::is_same_v<Shape, BoxCollider> ||
                              std::is_same_v<Shape, RampCollider>) {
                    output << ' ';
                    writeVec3(output, shape.halfExtents);
                } else if constexpr (std::is_same_v<Shape, SphereCollider>) {
                    output << ' ';
                    writeFloat(output, shape.radius);
                } else if constexpr (std::is_same_v<Shape, CapsuleCollider>) {
                    output << ' ';
                    writeFloat(output, shape.radius);
                    output << ' ';
                    writeFloat(output, shape.height);
                }
            }, collider.shape);
            output << '\n';
        }

        void writeRigidbody(std::ostream &output, const RigidbodyComponent &body) {
            output << static_cast<int>(body.type) << ' ';
            writeFloat(output, body.mass);
            output << ' ';
            writeFloat(output, body.linearDamping);
            output << ' ';
            writeFloat(output, body.angularDamping);
            output << ' ';
            output << static_cast<int>(body.useGravity) << ' '
                    << static_cast<int>(body.fixedRotation) << ' ';
            writeVec3(output, body.linearVelocity);
            output << ' ';
            writeVec3(output, body.angularVelocity);
            output << '\n';
        }

        RigidbodyComponent readRigidbody(std::istream &input) {
            RigidbodyComponent body;
            const int type = read<int>(input, "rigidbody type");
            if (type < static_cast<int>(RigidbodyType::Static) ||
                type > static_cast<int>(RigidbodyType::Kinematic)) {
                invalidScene("unknown rigidbody type");
            }
            body.type = static_cast<RigidbodyType>(type);
            body.mass = readFloat(input, "rigidbody mass");
            body.linearDamping = readFloat(input, "rigidbody linear damping");
            body.angularDamping = readFloat(input, "rigidbody angular damping");
            body.useGravity = readBool(input, "rigidbody gravity flag");
            body.fixedRotation = readBool(input, "rigidbody fixed-rotation flag");
            body.linearVelocity = readVec3(input, "rigidbody linear velocity");
            body.angularVelocity = readVec3(input, "rigidbody angular velocity");
            if (body.mass <= 0.0F || body.linearDamping < 0.0F || body.angularDamping < 0.0F) {
                invalidScene("rigidbody values are invalid");
            }
            return body;
        }

        ColliderComponent readCollider(std::istream &input) {
            const int type = read<int>(input, "collider shape");
            if (type < 0 || type > 4) {
                invalidScene("unknown collider shape");
            }
            ColliderComponent collider;
            collider.offset = readVec3(input, "collider offset");
            collider.isTrigger = readBool(input, "collider trigger flag");
            collider.friction = readFloat(input, "collider friction");
            collider.restitution = readFloat(input, "collider restitution");
            if (collider.friction < 0.0F || collider.restitution < 0.0F || collider.restitution > 1.0F) {
                invalidScene("collider material values are invalid");
            }
            if (type == 0) {
                collider.shape = BoxCollider{readVec3(input, "box collider half extents")};
                if (std::get<BoxCollider>(collider.shape).halfExtents.x() <= 0.0F ||
                    std::get<BoxCollider>(collider.shape).halfExtents.y() <= 0.0F ||
                    std::get<BoxCollider>(collider.shape).halfExtents.z() <= 0.0F) {
                    invalidScene("box collider half extents must be positive");
                }
            } else if (type == 1) {
                collider.shape = SphereCollider{readFloat(input, "sphere collider radius")};
                if (std::get<SphereCollider>(collider.shape).radius <= 0.0F) {
                    invalidScene("sphere collider radius must be positive");
                }
            } else if (type == 2) {
                collider.shape = CapsuleCollider{
                    readFloat(input, "capsule collider radius"),
                    readFloat(input, "capsule collider height"),
                };
                const auto &capsule = std::get<CapsuleCollider>(collider.shape);
                if (capsule.radius <= 0.0F || capsule.height <= 0.0F) {
                    invalidScene(
                        "capsule collider dimensions must be positive");
                }
            } else if (type == 3) {
                collider.shape = RampCollider{readVec3(input, "ramp collider half extents")};
                const auto &ramp = std::get<RampCollider>(collider.shape);
                if (ramp.halfExtents.x() <= 0.0F || ramp.halfExtents.y() <= 0.0F || ramp.halfExtents.z() <= 0.0F) {
                    invalidScene("ramp collider half extents must be positive");
                }
            } else {
                // The renderer mesh is assigned after all entity components
                // have been decoded, so its shared geometry is not duplicated
                // in the collider record.
                collider.shape = MeshCollider{};
            }
            return collider;
        }

        Particles::ParticleEmitter readParticleEmitter(std::istream &input) {
            Particles::ParticleEmitter emitter;
            emitter.position = readVec3(input, "particle emitter position");
            emitter.minVelocity = readVec3(input, "particle emitter minimum velocity");
            emitter.maxVelocity = readVec3(input, "particle emitter maximum velocity");
            emitter.color = readColorRgba(input, "particle emitter color");
            emitter.minLifeTime = readFloat(input, "particle emitter minimum lifetime");
            emitter.maxLifeTime = readFloat(input, "particle emitter maximum lifetime");
            emitter.minSize = readFloat(input, "particle emitter minimum size");
            emitter.maxSize = readFloat(input, "particle emitter maximum size");
            emitter.spawnRate = readFloat(input, "particle emitter spawn rate");
            emitter.accumulator = 0.0F;
            if (emitter.minLifeTime < 0.0F || emitter.maxLifeTime < emitter.minLifeTime ||
                emitter.minSize < 0.0F || emitter.maxSize < emitter.minSize ||
                emitter.spawnRate < 0.0F) {
                invalidScene("particle emitter settings are invalid");
            }
            return emitter;
        }

        Particles::SmokeEmitter readSmokeEmitter(std::istream &input) {
            Particles::SmokeEmitter emitter;
            static_cast<Particles::ParticleEmitter &>(emitter) = readParticleEmitter(input);
            emitter.buoyancy = readFloat(input, "smoke buoyancy");
            emitter.drag = readFloat(input, "smoke drag");
            emitter.turbulence = readFloat(input, "smoke turbulence");
            emitter.collisionRadius = readFloat(input, "smoke collision radius");
            if (emitter.buoyancy < 0.0F || emitter.drag < 0.0F || emitter.turbulence < 0.0F ||
                emitter.collisionRadius < 0.0F) {
                invalidScene("smoke emitter settings are invalid");
            }
            return emitter;
        }

        void writeProceduralCloud(std::ostream& output, const ProceduralCloudComponent& cloud) {
            output << cloud.seed << ' ' << cloud.puffCount << ' ';
            writeVec3(output, cloud.dimensions);
            output << ' ';
            writeFloat(output, cloud.puffRadius);
        }

        ProceduralCloudComponent readProceduralCloud(std::istream& input) {
            ProceduralCloudComponent cloud;
            cloud.seed = read<std::uint32_t>(input, "cloud seed");
            cloud.puffCount = read<std::uint32_t>(input, "cloud puff count");
            cloud.dimensions = readVec3(input, "cloud dimensions");
            cloud.puffRadius = readFloat(input, "cloud puff radius");
            if (cloud.puffCount == 0U || cloud.puffCount > 128U || cloud.dimensions.x() <= 0.0F ||
                cloud.dimensions.y() <= 0.0F || cloud.dimensions.z() <= 0.0F || cloud.puffRadius <= 0.0F) {
                invalidScene("procedural cloud settings are invalid");
            }
            return cloud;
        }

        void writeTerrain(std::ostream& output, const TerrainComponent& terrain) {
            output << terrain.resolution << ' ';
            writeFloat(output, terrain.width);
            output << ' ';
            writeFloat(output, terrain.depth);
            output << ' ';
            writeFloat(output, terrain.minimumHeight);
            output << ' ';
            writeFloat(output, terrain.maximumHeight);
            output << ' ' << terrain.heights.size();
            for (const float height : terrain.heights) {
                output << ' ';
                writeFloat(output, height);
            }
            output << ' ' << terrain.colors.size();
            for (const Vec3& color : terrain.colors) {
                output << ' ';
                writeVec3(output, color);
            }
            output << '\n';
        }

        TerrainComponent readTerrain(std::istream& input, const bool hasColors) {
            const auto resolution = read<std::uint32_t>(input, "terrain resolution");
            const float width = readFloat(input, "terrain width");
            const float depth = readFloat(input, "terrain depth");
            const float minimumHeight = readFloat(input, "terrain minimum height");
            const float maximumHeight = readFloat(input, "terrain maximum height");
            if (resolution < TerrainComponent::MinimumResolution ||
                resolution > TerrainComponent::MaximumResolution) {
                invalidScene("terrain resolution is outside the supported range");
            }
            TerrainComponent terrain{resolution, width, depth, minimumHeight, maximumHeight};
            const std::size_t count = readCount(input, "terrain height count",
                static_cast<std::size_t>(TerrainComponent::MaximumResolution) *
                TerrainComponent::MaximumResolution);
            if (count != terrain.sampleCount()) {
                invalidScene("terrain height count does not match its resolution");
            }
            for (float& height : terrain.heights) {
                height = readFloat(input, "terrain height");
            }
            if (hasColors) {
                const std::size_t colorCount = readCount(input, "terrain colour count", terrain.sampleCount());
                if (colorCount != terrain.sampleCount()) invalidScene("terrain colour count does not match its resolution");
                for (Vec3& color : terrain.colors) color = readVec3(input, "terrain colour");
            }
            if (!terrain.valid()) invalidScene("terrain data is invalid");
            return terrain;
        }

        void writeTerrainBinary(std::ostream& scene, std::ostream& data, const TerrainComponent& terrain) {
            const auto offset = data.tellp();
            if (offset < 0) throw std::runtime_error("Could not determine terrain data offset");
            const std::uint64_t byteCount = static_cast<std::uint64_t>(terrain.heights.size()) * 4ULL +
                                            static_cast<std::uint64_t>(terrain.colors.size()) * 12ULL;
            scene << "TERRAIN_BIN " << terrain.resolution << ' ';
            writeFloat(scene, terrain.width); scene << ' ';
            writeFloat(scene, terrain.depth); scene << ' ';
            writeFloat(scene, terrain.minimumHeight); scene << ' ';
            writeFloat(scene, terrain.maximumHeight);
            scene << ' ' << terrain.heights.size() << ' ' << terrain.colors.size() << ' '
                  << static_cast<std::uint64_t>(offset) << ' ' << byteCount << ' '
                  << terrainChecksum(terrain) << '\n';
            for (const float height : terrain.heights) writeLittleEndianU32(data, std::bit_cast<std::uint32_t>(height));
            for (const Vec3& color : terrain.colors) {
                writeLittleEndianU32(data, std::bit_cast<std::uint32_t>(color.x()));
                writeLittleEndianU32(data, std::bit_cast<std::uint32_t>(color.y()));
                writeLittleEndianU32(data, std::bit_cast<std::uint32_t>(color.z()));
            }
            if (!data) throw std::runtime_error("Could not write terrain data");
        }

        TerrainComponent readTerrainBinary(std::istream& scene, std::istream& data) {
            const auto resolution = read<std::uint32_t>(scene, "terrain resolution");
            const float width = readFloat(scene, "terrain width");
            const float depth = readFloat(scene, "terrain depth");
            const float minimumHeight = readFloat(scene, "terrain minimum height");
            const float maximumHeight = readFloat(scene, "terrain maximum height");
            if (resolution < TerrainComponent::MinimumResolution || resolution > TerrainComponent::MaximumResolution) {
                invalidScene("terrain resolution is outside the supported range");
            }
            TerrainComponent terrain{resolution, width, depth, minimumHeight, maximumHeight};
            const auto heightCount = readCount(scene, "terrain height count", terrain.sampleCount());
            const auto colorCount = readCount(scene, "terrain colour count", terrain.sampleCount());
            const auto offset = read<std::uint64_t>(scene, "terrain data offset");
            const auto byteCount = read<std::uint64_t>(scene, "terrain data size");
            const auto checksum = read<std::uint64_t>(scene, "terrain data checksum");
            const auto expectedBytes = static_cast<std::uint64_t>(heightCount) * 4ULL +
                                       static_cast<std::uint64_t>(colorCount) * 12ULL;
            if (heightCount != terrain.sampleCount() || colorCount != terrain.sampleCount() || byteCount != expectedBytes) {
                invalidScene("terrain binary data dimensions are invalid");
            }
            data.clear();
            data.seekg(static_cast<std::streamoff>(offset));
            if (!data) invalidScene("could not seek terrain data");
            for (float& height : terrain.heights) height = std::bit_cast<float>(readLittleEndianU32(data, "terrain height"));
            for (Vec3& color : terrain.colors) {
                // Decode in separate statements: function-argument evaluation
                // order must not determine the byte order of a colour.
                const float red = std::bit_cast<float>(readLittleEndianU32(data, "terrain colour red"));
                const float green = std::bit_cast<float>(readLittleEndianU32(data, "terrain colour green"));
                const float blue = std::bit_cast<float>(readLittleEndianU32(data, "terrain colour blue"));
                color = {red, green, blue};
            }
            if (terrainChecksum(terrain) != checksum) invalidScene("terrain binary data checksum does not match");
            if (!terrain.valid()) invalidScene("terrain binary values are invalid");
            return terrain;
        }

        void writeImageBinary(std::ostream& scene, std::ostream& data, const std::uint32_t width,
                              const std::uint32_t height, const std::vector<std::uint8_t>& rgbaPixels) {
            const auto offset = data.tellp();
            if (offset < 0) throw std::runtime_error("Could not determine embedded image data offset");
            scene << "IMAGE_BIN " << width << ' ' << height << ' ' << rgbaPixels.size() << ' '
                  << static_cast<std::uint64_t>(offset) << ' ' << byteChecksum(rgbaPixels) << '\n';
            data.write(reinterpret_cast<const char*>(rgbaPixels.data()), static_cast<std::streamsize>(rgbaPixels.size()));
            if (!data) throw std::runtime_error("Could not write embedded image data");
        }

        Mesh::Image readImageBinary(std::istream& scene, std::istream& data) {
            Mesh::Image image;
            image.width = read<std::uint32_t>(scene, "image width");
            image.height = read<std::uint32_t>(scene, "image height");
            const auto byteCount = readCount(scene, "image byte count", MaxIndices * 4);
            const auto offset = read<std::uint64_t>(scene, "image data offset");
            const auto checksum = read<std::uint64_t>(scene, "image data checksum");
            if (image.width == 0 || image.height == 0 ||
                byteCount != static_cast<std::size_t>(image.width) * image.height * 4) {
                invalidScene("image dimensions do not match RGBA pixel data");
            }
            image.rgbaPixels.resize(byteCount);
            data.clear();
            data.seekg(static_cast<std::streamoff>(offset));
            if (!data) invalidScene("could not seek embedded image data");
            data.read(reinterpret_cast<char*>(image.rgbaPixels.data()), static_cast<std::streamsize>(byteCount));
            if (!data || byteChecksum(image.rgbaPixels) != checksum) invalidScene("embedded image data is invalid");
            return image;
        }

        void writeTerrainGrass(std::ostream& output, const TerrainGrassComponent& grass,
                               const std::size_t meshId) {
            output << meshId << ' ';
            writeMaterial(output, grass.material);
            output << ' ' << static_cast<int>(grass.castShadow) << ' ' << grass.instances.size();
            for (const auto& instance : grass.instances) {
                output << ' ';
                writeVec3(output, instance.position);
                output << ' ';
                writeFloat(output, instance.yaw);
                output << ' ';
                writeFloat(output, instance.scale);
                output << ' ';
                writeFloat(output, instance.bendX);
                output << ' ';
                writeFloat(output, instance.bendZ);
                output << ' ';
                writeFloat(output, instance.trampled);
            }
            output << '\n';
        }

        std::vector<Entity> sortedEntities(const Registry &registry) {
            std::vector<Entity> entities;
            entities.reserve(registry.size());
            registry.view<>([&entities](const Entity entity) {
                entities.push_back(entity);
            });
            std::ranges::sort(entities);
            return entities;
        }
    } // namespace

    void SceneSerializer::save(const Scene &scene, const std::filesystem::path &path) {
        save(scene.registry(), path);
    }

    void SceneSerializer::save(const Scene &scene, const std::filesystem::path &path,
                               const std::uint32_t msaaSamples) {
        save(scene.registry(), path, msaaSamples);
    }

    void SceneSerializer::save(const Scene &scene, std::ostream &output) {
        save(scene.registry(), output);
    }

    void SceneSerializer::save(const Scene &scene, std::ostream &output,
                               const std::uint32_t msaaSamples) {
        save(scene.registry(), output, msaaSamples);
    }

    void SceneSerializer::load(Scene &scene, const std::filesystem::path &path) {
        scene.detachObjectHandles();
        try {
            load(scene.registry(), path);
        } catch (...) {
            // Registry loading is transactional. Reconnect the still-valid
            // previous registry to its GameObject wrappers before propagating
            // the actual load error to the editor.
            scene.rebuildObjectHandles();
            throw;
        }
        scene.rebuildObjectHandles();
    }

    void SceneSerializer::load(Scene &scene, const std::filesystem::path &path,
                               std::optional<std::uint32_t> &msaaSamples) {
        scene.detachObjectHandles();
        try {
            load(scene.registry(), path, msaaSamples);
        } catch (...) {
            scene.rebuildObjectHandles();
            throw;
        }
        scene.rebuildObjectHandles();
    }

    void SceneSerializer::load(Scene &scene, std::istream &input) {
        scene.detachObjectHandles();
        try {
            load(scene.registry(), input);
        } catch (...) {
            scene.rebuildObjectHandles();
            throw;
        }
        scene.rebuildObjectHandles();
    }

    void SceneSerializer::load(Scene &scene, std::istream &input,
                               std::optional<std::uint32_t> &msaaSamples) {
        scene.detachObjectHandles();
        try {
            load(scene.registry(), input, msaaSamples);
        } catch (...) {
            scene.rebuildObjectHandles();
            throw;
        }
        scene.rebuildObjectHandles();
    }

    void SceneSerializer::save(const Registry &registry,
                               const std::filesystem::path &path) {
        save(registry, path, 0);
    }

    void SceneSerializer::save(const Registry &registry,
                               const std::filesystem::path &path,
                               const std::uint32_t msaaSamples) {
        std::ofstream output(path);
        if (!output) {
            throw std::runtime_error("Could not open scene for writing: " + path.string());
        }
        const auto dataPath = terrainDataPath(path);
        std::ofstream terrainData(dataPath, std::ios::binary | std::ios::trunc);
        if (!terrainData) throw std::runtime_error("Could not open terrain data for writing: " + dataPath.string());
        terrainData.write(TerrainDataMagic.data(), static_cast<std::streamsize>(TerrainDataMagic.size()));
        writeLittleEndianU32(terrainData, TerrainDataVersion);
        output.pword(terrainDataStreamSlot()) = &terrainData;
        save(registry, output, msaaSamples);
        output.pword(terrainDataStreamSlot()) = nullptr;
        if (!output) {
            throw std::runtime_error("Could not finish writing scene: " + path.string());
        }
        if (!terrainData) throw std::runtime_error("Could not finish writing terrain data: " + dataPath.string());
    }

    void SceneSerializer::save(const Registry &registry, std::ostream &output) {
        save(registry, output, 0);
    }

    // The serializer's format branches are kept together to make the output
    // grammar explicit and preserve field ordering.
    // NOLINTNEXTLINE(readability-function-cognitive-complexity)
    void SceneSerializer::save(const Registry &registry, std::ostream &output,
                               const std::uint32_t msaaSamples) {
        output.imbue(std::locale::classic());
        std::ostream& serialized = output;
        auto* terrainData = static_cast<std::ostream*>(output.pword(terrainDataStreamSlot()));
        const std::vector<Entity> entities = sortedEntities(registry);
        std::vector<std::shared_ptr<const Mesh> > meshes;
        std::unordered_map<const Mesh *, std::size_t> meshIds;

        for (const Entity entity: entities) {
            if (!registry.has<MeshRenderer>(entity)) {
                continue;
            }
            // Terrain geometry is deterministic from its compact heightmap and
            // is rebuilt while loading instead of being duplicated in MESHES.
            if (registry.has<TerrainComponent>(entity)) continue;
            const auto &renderer = registry.get<MeshRenderer>(entity);
            if (renderer.mesh && !meshIds.contains(renderer.mesh.get())) {
                meshIds.emplace(renderer.mesh.get(), meshes.size());
                meshes.push_back(renderer.mesh);
            }
        }
        for (const Entity entity : entities) {
            if (!registry.has<TerrainGrassComponent>(entity)) continue;
            const auto& grass = registry.get<TerrainGrassComponent>(entity);
            if (grass.mesh && !meshIds.contains(grass.mesh.get())) {
                meshIds.emplace(grass.mesh.get(), meshes.size());
                meshes.push_back(grass.mesh);
            }
        }

        if (msaaSamples != 0 && msaaSamples != 2 && msaaSamples != 4) {
            throw std::invalid_argument("MSAA samples must be 0, 2 or 4");
        }
        serialized << "GAMENGINE_SCENE " << (terrainData ? FormatVersion : LegacyFormatVersion) << '\n';
        serialized << "SETTINGS MSAA " << msaaSamples << '\n';
        serialized << "MESHES " << meshes.size() << '\n';
        for (std::size_t meshId = 0; meshId < meshes.size(); ++meshId) {
            const Mesh &mesh = *meshes[meshId];
            if (!mesh.sourcePath.empty()) {
                serialized << "MESH_ASSET " << meshId << ' '
                           << std::quoted(mesh.sourcePath.lexically_normal().generic_string()) << '\n';
                continue;
            }
            serialized << "MESH " << meshId << ' ' << mesh.vertices.size() << ' '
                    << mesh.indices.size() << ' ' << mesh.materials.size() << ' '
                    << mesh.images.size() << '\n';
            for (const Vertex &vertex: mesh.vertices) {
                serialized << "VERTEX ";
                writeVec3(serialized, vertex.position);
                serialized << ' ';
                writeVec3(serialized, vertex.color);
                serialized << ' ';
                writeFloat(serialized, vertex.texCoord.x());
                serialized << ' ';
                writeFloat(serialized, vertex.texCoord.y());
                serialized << ' ';
                writeVec3(serialized, vertex.normal);
                serialized << ' ';
                writeFloat(serialized, vertex.tangent.x());
                serialized << ' ';
                writeFloat(serialized, vertex.tangent.y());
                serialized << ' ';
                writeFloat(serialized, vertex.tangent.z());
                serialized << ' ';
                writeFloat(serialized, vertex.tangent.w());
                serialized << ' ' << vertex.materialIndex;
                serialized << '\n';
            }
            serialized << "INDICES";
            for (const std::uint32_t index: mesh.indices) {
                serialized << ' ' << index;
            }
            serialized << '\n';
            for (const PBRMaterial &material: mesh.materials) {
                serialized << "MATERIAL ";
                writeMaterial(serialized, material);
                serialized << '\n';
            }
            for (const auto &[width, height, rgbaPixels]: mesh.images) {
                if (terrainData) {
                    writeImageBinary(serialized, *terrainData, width, height, rgbaPixels);
                    continue;
                }
                serialized << "IMAGE " << width << ' ' << height << ' '
                        << rgbaPixels.size() << '\n' << "PIXELS";
                for (const std::uint8_t pixel: rgbaPixels) {
                    serialized << ' ' << static_cast<unsigned>(pixel);
                }
                serialized << '\n';
            }
        }

        serialized << "ENTITIES " << entities.size() << '\n';
        for (const Entity entity: entities) {
            serialized << "ENTITY\n";
            const UUID uuid = registry.has<UUIDComponent>(entity)
                                  ? registry.get<UUIDComponent>(entity).value
                                  : entity;
            const std::string name = registry.has<NameComponent>(entity)
                                         ? registry.get<NameComponent>(entity).value
                                         : "Entity " + std::to_string(entityIndex(entity));
            serialized << "IDENTITY " << uuid << ' ' << std::quoted(name) << '\n';
            if (registry.has<ParentComponent>(entity)) {
                serialized << "PARENT " << registry.get<ParentComponent>(entity).parent << '\n';
            }
            if (registry.has<HierarchyOrderComponent>(entity)) {
                serialized << "HIERARCHY_ORDER "
                           << registry.get<HierarchyOrderComponent>(entity).value << '\n';
            }
            if (registry.has<Transform>(entity)) {
                const auto &transform = registry.get<Transform>(entity);
                serialized << "TRANSFORM ";
                writeVec3(serialized, transform.position);
                serialized << ' ';
                writeVec3(serialized, transform.rotation);
                serialized << ' ';
                writeVec3(serialized, transform.scale);
                serialized << '\n';
            }
            if (registry.has<ColliderComponent>(entity)) {
                serialized << "COLLIDER ";
                writeCollider(serialized, registry.get<ColliderComponent>(entity));
            }
            if (registry.has<RigidbodyComponent>(entity)) {
                serialized << "RIGIDBODY ";
                writeRigidbody(serialized, registry.get<RigidbodyComponent>(entity));
            }
            if (registry.has<TerrainComponent>(entity)) {
                if (terrainData) {
                    writeTerrainBinary(serialized, *terrainData, registry.get<TerrainComponent>(entity));
                } else {
                    serialized << "TERRAIN_V2 ";
                    writeTerrain(serialized, registry.get<TerrainComponent>(entity));
                }
            }
            if (registry.has<TerrainGrassComponent>(entity)) {
                const auto& grass = registry.get<TerrainGrassComponent>(entity);
                if (grass.mesh) {
                    serialized << "TERRAIN_GRASS_V2 ";
                    writeTerrainGrass(serialized, grass, meshIds.at(grass.mesh.get()));
                }
            }
            if (registry.has<MeshRenderer>(entity)) {
                const auto &renderer = registry.get<MeshRenderer>(entity);
                const long long meshId = renderer.mesh && !registry.has<TerrainComponent>(entity)
                                             ? static_cast<long long>(meshIds.at(renderer.mesh.get()))
                                             : -1;
                serialized << "MESH_RENDERER " << meshId << ' ';
                writeMaterial(serialized, renderer.material);
                serialized << ' ' << static_cast<int>(renderer.castShadow) << ' '
                        << renderer.cullingBatch << '\n';
            }
            if (registry.has<LightComponent>(entity)) {
                const auto &light = registry.get<LightComponent>(entity);
                serialized << "LIGHT " << static_cast<int>(light.type) << ' ';
                writeColor(serialized, light.color);
                serialized << ' ';
                writeFloat(serialized, light.intensity);
                serialized << ' ';
                writeFloat(serialized, light.range);
                serialized << ' ';
                writeFloat(serialized, light.innerConeAngle);
                serialized << ' ';
                writeFloat(serialized, light.outerConeAngle);
                serialized << ' ' << static_cast<int>(light.enabled) << ' '
                        << static_cast<int>(light.castShadows) << '\n';
            }
            if (registry.has<WindComponent>(entity)) {
                const auto& wind = registry.get<WindComponent>(entity);
                serialized << "WIND_V2 ";
                writeVec3(serialized, wind.direction);
                serialized << ' ';
                writeFloat(serialized, wind.strength);
                serialized << ' ';
                writeFloat(serialized, wind.gustStrength);
                serialized << ' ';
                writeFloat(serialized, wind.frequency);
                serialized << ' ';
                writeFloat(serialized, wind.range);
                serialized << ' ' << static_cast<int>(wind.enabled) << '\n';
            }
            if (registry.has<CameraComponent>(entity)) {
                const auto &camera = registry.get<CameraComponent>(entity);
                serialized << "CAMERA " << static_cast<int>(camera.projection) << ' ';
                writeFloat(serialized, camera.fieldOfView);
                serialized << ' ';
                writeFloat(serialized, camera.orthographicSize);
                serialized << ' ';
                writeFloat(serialized, camera.nearClip);
                serialized << ' ';
                writeFloat(serialized, camera.farClip);
                serialized << ' ';
                writeFloat(serialized, camera.aspectRatio);
                serialized << ' ' << static_cast<int>(camera.primary) << '\n';
            }
            if (registry.has<ColorPickerComponent>(entity)) {
                serialized << "COLOR_PICKER ";
                writeColorRgba(serialized, registry.get<ColorPickerComponent>(entity).color);
                serialized << '\n';
            }
            if (registry.has<ParticleEmitterComponent>(entity)) {
                serialized << "PARTICLE_EMITTER ";
                writeParticleEmitter(serialized,
                                     registry.get<ParticleEmitterComponent>(entity).emitter);
                serialized << '\n';
            }
            if (registry.has<SmokeEmitterComponent>(entity)) {
                serialized << "SMOKE_EMITTER ";
                writeSmokeEmitter(serialized,
                                  registry.get<SmokeEmitterComponent>(entity).emitter);
                serialized << '\n';
            }
            if (registry.has<ProceduralCloudComponent>(entity)) {
                serialized << "PROCEDURAL_CLOUD ";
                writeProceduralCloud(serialized, registry.get<ProceduralCloudComponent>(entity));
                serialized << '\n';
            }
            if (registry.has<ScriptComponent>(entity)) {
                const auto &script = registry.get<ScriptComponent>(entity);
                serialized << "SCRIPT " << std::quoted(script.className) << ' '
                        << static_cast<int>(script.enabled) << '\n';
            }
            serialized << "END_ENTITY\n";
        }
        serialized << "END_SCENE\n";

        if (!output) {
            throw std::runtime_error("Could not write serialized scene");
        }
    }

    void SceneSerializer::load(Registry &registry,
                               const std::filesystem::path &path) {
        std::optional<std::uint32_t> ignoredMsaa;
        load(registry, path, ignoredMsaa);
    }

    void SceneSerializer::load(Registry &registry,
                               const std::filesystem::path &path,
                               std::optional<std::uint32_t> &msaaSamples) {
        std::ifstream input(path);
        if (!input) {
            throw std::runtime_error("Could not open scene for reading: " + path.string());
        }
        const auto dataPath = terrainDataPath(path);
        std::ifstream terrainData(dataPath, std::ios::binary);
        // A legacy scene does not need a sidecar.  The decoder below reports a
        // precise error if a v12 terrain record is encountered without one.
        if (terrainData) {
            std::array<char, TerrainDataMagic.size()> magic{};
            terrainData.read(magic.data(), static_cast<std::streamsize>(magic.size()));
            if (magic != TerrainDataMagic || readLittleEndianU32(terrainData, "terrain data version") != TerrainDataVersion) {
                invalidScene("unsupported terrain data sidecar");
            }
            input.pword(terrainDataStreamSlot()) = &terrainData;
        }
        load(registry, input, msaaSamples);
        input.pword(terrainDataStreamSlot()) = nullptr;
    }

    void SceneSerializer::load(Registry &registry, std::istream &input) {
        std::optional<std::uint32_t> ignoredMsaa;
        load(registry, input, ignoredMsaa);
    }

    // The scene format is intentionally decoded in one transaction so that a
    // partially loaded registry is never exposed to the caller. The branches
    // below are format validation and component dispatch, rather than
    // independent business logic that can be simplified without obscuring
    // the on-disk format.
    // NOLINTNEXTLINE(readability-function-cognitive-complexity)
    void SceneSerializer::load(Registry &registry, std::istream &input,
                               std::optional<std::uint32_t> &msaaSamples) {
        msaaSamples.reset();
        input.imbue(std::locale::classic());
        expect(input, "GAMENGINE_SCENE");
        const auto version = read<unsigned>(input, "format version");
        if (version != LegacyFormatVersion && version != FormatVersion) {
            invalidScene("unsupported format version " + std::to_string(version));
        }
        auto* terrainData = static_cast<std::istream*>(input.pword(terrainDataStreamSlot()));

        std::string section;
        input >> section;
        if (!input) {
            invalidScene("expected 'MESHES'");
        }
        if (section == "SETTINGS") {
            expect(input, "MSAA");
            const auto samples = read<unsigned>(input, "MSAA sample count");
            if (samples != 0 && samples != 2 && samples != 4) {
                invalidScene("MSAA sample count must be 0, 2 or 4");
            }
            msaaSamples = samples;
            expect(input, "MESHES");
        } else if (section != "MESHES") {
            invalidScene("expected 'MESHES'");
        }
        const std::size_t meshCount = readCount(input, "mesh count", MaxMeshes);
        std::vector<std::shared_ptr<const Mesh> > meshes;
        meshes.reserve(meshCount);
        Assets::AssetManager assetManager;
        for (std::size_t meshIndex = 0; meshIndex < meshCount; ++meshIndex) {
            const auto recordType = read<std::string>(input, "mesh record type");
            if (recordType != "MESH" && recordType != "MESH_ASSET") {
                invalidScene("expected 'MESH' or 'MESH_ASSET'");
            }
            const std::size_t serializedId = readCount(input, "mesh id", MaxMeshes);
            if (serializedId != meshIndex) {
                invalidScene("mesh identifiers must be contiguous");
            }
            if (recordType == "MESH_ASSET") {
                std::string sourcePath;
                input >> std::quoted(sourcePath);
                if (!input || sourcePath.empty()) {
                    invalidScene("could not read mesh asset path");
                }
                auto mesh = assetManager.loadMesh(std::filesystem::path{sourcePath}).shared();
                if (!mesh) {
                    invalidScene("could not load mesh asset '" + sourcePath + "'");
                }
                meshes.push_back(std::move(mesh));
                continue;
            }
            const std::size_t vertexCount = readCount(input, "vertex count", MaxVertices);
            const std::size_t indexCount = readCount(input, "index count", MaxIndices);
            const std::size_t materialCount = readCount(input, "material count", MaxMeshes);
            const std::size_t imageCount = readCount(input, "image count", MaxMeshes);

            auto mesh = std::make_shared<Mesh>();
            mesh->vertices.reserve(vertexCount);
            for (std::size_t vertexIndex = 0; vertexIndex < vertexCount; ++vertexIndex) {
                expect(input, "VERTEX");
                Vertex vertex;
                vertex.position = readVec3(input, "vertex position");
                vertex.color = readVec3(input, "vertex color");
                vertex.texCoord = {
                    readFloat(input, "texture coordinate"),
                    readFloat(input, "texture coordinate"),
                };
                vertex.normal = readVec3(input, "vertex normal");
                // Read sequentially before construction. MSVC may evaluate the
                // conversion used by braced assignment right-to-left, which
                // swapped tangent.x with tangent.w during scene round-trips.
                const float tangentX = readFloat(input, "tangent x");
                const float tangentY = readFloat(input, "tangent y");
                const float tangentZ = readFloat(input, "tangent z");
                const float tangentW = readFloat(input, "tangent handedness");
                vertex.tangent = Vec4{tangentX, tangentY, tangentZ, tangentW};
                vertex.materialIndex = read<std::uint32_t>(input, "vertex material index");
                mesh->vertices.push_back(vertex);
            }
            expect(input, "INDICES");
            mesh->indices.reserve(indexCount);
            for (std::size_t index = 0; index < indexCount; ++index) {
                const auto value = read<unsigned long long>(input, "mesh index");
                if (value > std::numeric_limits<std::uint32_t>::max() ||
                    value >= mesh->vertices.size()) {
                    invalidScene("mesh index is outside the vertex array");
                }
                mesh->indices.push_back(static_cast<std::uint32_t>(value));
            }
            mesh->materials.reserve(materialCount);
            for (std::size_t material = 0; material < materialCount; ++material) {
                expect(input, "MATERIAL");
                mesh->materials.push_back(readMaterial(input));
            }
            mesh->images.reserve(imageCount);
            for (std::size_t image = 0; image < imageCount; ++image) {
                const auto imageRecord = read<std::string>(input, "image record type");
                if (imageRecord == "IMAGE_BIN") {
                    if (version != FormatVersion || terrainData == nullptr) {
                        invalidScene("embedded image data sidecar is missing");
                    }
                    mesh->images.push_back(readImageBinary(input, *terrainData));
                    continue;
                }
                if (imageRecord != "IMAGE") invalidScene("expected 'IMAGE' or 'IMAGE_BIN'");
                Mesh::Image value;
                value.width = read<std::uint32_t>(input, "image width");
                value.height = read<std::uint32_t>(input, "image height");
                const std::size_t pixelCount = readCount(input, "image pixel count", MaxIndices * 4);
                if (value.width == 0 || value.height == 0 || pixelCount != static_cast<std::size_t>(value.width) * value
                    .height * 4) {
                    invalidScene("image dimensions do not match RGBA pixel data");
                }
                expect(input, "PIXELS");
                value.rgbaPixels.reserve(pixelCount);
                for (std::size_t pixel = 0; pixel < pixelCount; ++pixel) {
                    const auto byte = read<unsigned>(input, "image pixel");
                    if (byte > std::numeric_limits<std::uint8_t>::max()) {
                        invalidScene("image pixel is outside byte range");
                    }
                    value.rgbaPixels.push_back(static_cast<std::uint8_t>(byte));
                }
                mesh->images.push_back(std::move(value));
            }
            meshes.push_back(std::move(mesh));
        }

        expect(input, "ENTITIES");
        const std::size_t entityCount = readCount(input, "entity count", MaxEntities);
        Registry loaded;
        for (std::size_t entityIndex = 0; entityIndex < entityCount; ++entityIndex) {
            expect(input, "ENTITY");
            const Entity entity = loaded.create();
            bool hasTransform = false;
            bool hasRenderer = false;
            bool hasLight = false;
            bool hasWind = false;
            bool hasCamera = false;
            bool hasScript = false;
            bool hasColorPicker = false;
            bool hasParticleEmitter = false;
            bool hasSmokeEmitter = false;
            bool hasProceduralCloud = false;
            bool hasCollider = false;
            bool hasRigidbody = false;
            bool hasTerrain = false;
            bool hasTerrainGrass = false;
            bool hasIdentity = false;
            bool hasParent = false;
            bool hasHierarchyOrder = false;

            while (true) {
                const auto component = read<std::string>(input, "component name");
                if (component == "END_ENTITY") {
                    break;
                }
                if (component == "IDENTITY") {
                    if (hasIdentity) {
                        invalidScene("entity contains an invalid IDENTITY component");
                    }
                    UUIDComponent uuid;
                    uuid.value = read<UUID>(input, "object UUID");
                    if (uuid.value == NullUUID) {
                        invalidScene("object UUID cannot be zero");
                    }
                    NameComponent name;
                    input >> std::quoted(name.value);
                    if (!input || name.value.empty()) {
                        invalidScene("could not read a non-empty object name");
                    }
                    loaded.add<UUIDComponent>(entity, uuid);
                    loaded.add<NameComponent>(entity, std::move(name));
                    reserveUUID(uuid.value);
                    hasIdentity = true;
                } else if (component == "PARENT") {
                    if (hasParent) {
                        invalidScene("entity contains an invalid PARENT component");
                    }
                    const UUID parent = read<UUID>(input, "parent UUID");
                    if (parent == NullUUID) {
                        invalidScene("parent UUID cannot be zero");
                    }
                    loaded.add<ParentComponent>(entity, ParentComponent{.parent = parent});
                    hasParent = true;
                } else if (component == "HIERARCHY_ORDER") {
                    if (hasHierarchyOrder) {
                        invalidScene("entity contains an invalid HIERARCHY_ORDER component");
                    }
                    loaded.add<HierarchyOrderComponent>(
                        entity, HierarchyOrderComponent{.value = read<std::uint32_t>(input, "hierarchy order")});
                    hasHierarchyOrder = true;
                } else if (component == "TRANSFORM") {
                    if (hasTransform) {
                        invalidScene("entity contains more than one Transform");
                    }
                    hasTransform = true;
                    loaded.add<Transform>(entity, Transform{
                                              .position = readVec3(input, "transform position"),
                                              .rotation = readVec3(input, "transform rotation"),
                                              .scale = readVec3(input, "transform scale"),
                                          });
                } else if (component == "COLLIDER") {
                    if (hasCollider) {
                        invalidScene("entity contains an invalid ColliderComponent");
                    }
                    hasCollider = true;
                    loaded.add<ColliderComponent>(entity, readCollider(input));
                } else if (component == "RIGIDBODY") {
                    if (hasRigidbody) {
                        invalidScene("entity contains an invalid RigidbodyComponent");
                    }
                    hasRigidbody = true;
                    loaded.add<RigidbodyComponent>(entity, readRigidbody(input));
                } else if (component == "TERRAIN" || component == "TERRAIN_V2" || component == "TERRAIN_BIN") {
                    if (hasTerrain) {
                        invalidScene("entity contains more than one TerrainComponent");
                    }
                    hasTerrain = true;
                    TerrainComponent terrain;
                    if (component == "TERRAIN_BIN") {
                        if (version != FormatVersion || terrainData == nullptr) {
                            invalidScene("terrain binary data sidecar is missing");
                        }
                        terrain = readTerrainBinary(input, *terrainData);
                    } else {
                        terrain = readTerrain(input, component == "TERRAIN_V2");
                    }
                    loaded.add<TerrainComponent>(entity, std::move(terrain));
                } else if (component == "TERRAIN_GRASS" || component == "TERRAIN_GRASS_V2") {
                    if (hasTerrainGrass) invalidScene("entity contains more than one TerrainGrassComponent");
                    hasTerrainGrass = true;
                    const std::size_t meshId = readCount(input, "grass mesh reference", meshes.size());
                    if (meshId >= meshes.size()) invalidScene("grass references an unknown mesh");
                    TerrainGrassComponent grass;
                    grass.mesh = meshes[meshId];
                    grass.material = readMaterial(input);
                    grass.castShadow = readBool(input, "grass cast-shadow flag");
                    const std::size_t count = readCount(input, "grass instance count",
                                                        TerrainGrassComponent::MaximumInstances);
                    grass.instances.reserve(count);
                    for (std::size_t i = 0; i < count; ++i) {
                        TerrainGrassInstance instance;
                        instance.position = readVec3(input, "grass instance position");
                        instance.yaw = readFloat(input, "grass instance yaw");
                        instance.scale = readFloat(input, "grass instance scale");
                        if (component == "TERRAIN_GRASS_V2") {
                            instance.bendX = readFloat(input, "grass instance bend x");
                            instance.bendZ = readFloat(input, "grass instance bend z");
                            instance.trampled = readFloat(input, "grass instance trample amount");
                        }
                        if (instance.scale <= 0.0F || instance.trampled < 0.0F ||
                            instance.trampled > 1.0F ||
                            std::hypot(instance.bendX, instance.bendZ) > 1.001F) {
                            invalidScene("grass instance deformation is invalid");
                        }
                        grass.instances.push_back(instance);
                    }
                    loaded.add<TerrainGrassComponent>(entity, std::move(grass));
                } else if (component == "MESH_RENDERER") {
                    if (hasRenderer) {
                        invalidScene("entity contains more than one MeshRenderer");
                    }
                    if (hasLight) {
                        invalidScene("a LightComponent cannot have a MeshRenderer");
                    }
                    hasRenderer = true;
                    const auto meshId = read<long long>(input, "mesh reference");
                    if (meshId < -1 || (meshId >= 0 &&
                                        static_cast<std::size_t>(meshId) >= meshes.size())) {
                        invalidScene("MeshRenderer references an unknown mesh");
                    }
                    MeshRenderer renderer;
                    if (meshId >= 0) {
                        renderer.mesh = meshes[static_cast<std::size_t>(meshId)];
                    }
                    renderer.material = readMaterial(input);
                    renderer.castShadow = readBool(input, "cast-shadow flag");
                    renderer.cullingBatch = read<std::uint32_t>(input, "culling batch");
                    loaded.add<MeshRenderer>(entity, std::move(renderer));
                } else if (component == "WIND" || component == "WIND_V2") {
                    if (hasWind) invalidScene("entity contains more than one WindComponent");
                    hasWind = true;
                    WindComponent wind;
                    wind.direction = readVec3(input, "wind direction");
                    wind.strength = readFloat(input, "wind strength");
                    wind.gustStrength = readFloat(input, "wind gust strength");
                    wind.frequency = readFloat(input, "wind frequency");
                    if (component == "WIND_V2")
                        wind.range = readFloat(input, "wind range");
                    wind.enabled = readBool(input, "wind enabled flag");
                    if (wind.direction.length() <= 1.0e-4F || wind.strength < 0.0F ||
                        wind.gustStrength < 0.0F || wind.frequency < 0.0F || wind.range <= 0.0F) {
                        invalidScene("wind settings are invalid");
                    }
                    loaded.add<WindComponent>(entity, wind);
                } else if (component == "LIGHT") {
                    if (hasLight) {
                        invalidScene("entity contains more than one LightComponent");
                    }
                    if (hasRenderer) {
                        invalidScene("a LightComponent cannot have a MeshRenderer");
                    }
                    hasLight = true;
                    const int type = read<int>(input, "light type");
                    if (type < static_cast<int>(LightType::Directional) ||
                        type > static_cast<int>(LightType::Spot)) {
                        invalidScene("unknown light type");
                    }
                    LightComponent light;
                    light.type = static_cast<LightType>(type);
                    light.color = readColor(input, "light color");
                    light.intensity = readFloat(input, "light intensity");
                    light.range = readFloat(input, "light range");
                    light.innerConeAngle = readFloat(input, "spot inner cone angle");
                    light.outerConeAngle = readFloat(input, "spot outer cone angle");
                    light.enabled = readBool(input, "light enabled flag");
                    light.castShadows = readBool(input, "light cast-shadows flag");
                    if (light.range <= 0.0F || light.innerConeAngle < 0.0F ||
                        light.outerConeAngle <= 0.0F ||
                        light.innerConeAngle > light.outerConeAngle ||
                        light.outerConeAngle >= 90.0F) {
                        invalidScene("local light settings are invalid");
                    }
                    loaded.add<LightComponent>(entity, light);
                } else if (component == "CAMERA") {
                    if (hasCamera) {
                        invalidScene("entity contains more than one CameraComponent");
                    }
                    hasCamera = true;
                    const int projection = read<int>(input, "camera projection");
                    if (projection < static_cast<int>(CameraProjection::Perspective) ||
                        projection > static_cast<int>(CameraProjection::Orthographic)) {
                        invalidScene("unknown camera projection");
                    }
                    CameraComponent camera;
                    camera.projection = static_cast<CameraProjection>(projection);
                    camera.fieldOfView = readFloat(input, "camera field of view");
                    camera.orthographicSize = readFloat(input, "camera orthographic size");
                    camera.nearClip = readFloat(input, "camera near clip");
                    camera.farClip = readFloat(input, "camera far clip");
                    camera.aspectRatio = readFloat(input, "camera aspect ratio");
                    camera.primary = readBool(input, "camera primary flag");
                    if (!camera.isValid()) {
                        invalidScene("camera settings are invalid");
                    }
                    loaded.add<CameraComponent>(entity, camera);
                } else if (component == "SCRIPT") {
                    if (hasScript) {
                        invalidScene("entity contains more than one ScriptComponent");
                    }
                    hasScript = true;
                    ScriptComponent script;
                    input >> std::quoted(script.className);
                    if (!input) {
                        invalidScene("could not read script class name");
                    }
                    script.enabled = readBool(input, "script enabled flag");
                    loaded.add<ScriptComponent>(entity, std::move(script));
                } else if (component == "COLOR_PICKER") {
                    if (hasColorPicker) {
                        invalidScene("entity contains more than one ColorPickerComponent");
                    }
                    hasColorPicker = true;
                    loaded.add<ColorPickerComponent>(entity, ColorPickerComponent{
                                                         .color = readColorRgba(input, "color picker color"),
                                                     });
                } else if (component == "PARTICLE_EMITTER") {
                    if (hasParticleEmitter) {
                        invalidScene("entity contains more than one ParticleEmitterComponent");
                    }
                    hasParticleEmitter = true;
                    loaded.add<ParticleEmitterComponent>(entity,
                                                         ParticleEmitterComponent{
                                                             .emitter = readParticleEmitter(input),
                                                         });
                } else if (component == "SMOKE_EMITTER") {
                    if (hasSmokeEmitter || hasParticleEmitter) {
                        invalidScene("entity contains an invalid SmokeEmitterComponent");
                    }
                    hasSmokeEmitter = true;
                    loaded.add<SmokeEmitterComponent>(entity,
                                                      SmokeEmitterComponent{.emitter = readSmokeEmitter(input)});
                } else if (component == "PROCEDURAL_CLOUD") {
                    if (hasProceduralCloud) {
                        invalidScene("entity contains more than one ProceduralCloudComponent");
                    }
                    hasProceduralCloud = true;
                    loaded.add<ProceduralCloudComponent>(entity, readProceduralCloud(input));
                } else {
                    invalidScene("unknown component '" + component + "'");
                }
            }
            if (!hasIdentity) {
                invalidScene("entity is missing IDENTITY");
            }
            if (hasTerrain) {
                if (!hasRenderer) invalidScene("terrain entity is missing MeshRenderer");
                const auto mesh = std::make_shared<Mesh>(loaded.get<TerrainComponent>(entity).createMesh());
                loaded.get<MeshRenderer>(entity).mesh = mesh;
                loaded.get<MeshRenderer>(entity).material.terrainLayered = true;
                if (hasCollider) {
                    if (auto* meshCollider = std::get_if<MeshCollider>(
                            &loaded.get<ColliderComponent>(entity).shape)) {
                        meshCollider->mesh = mesh;
                    }
                }
            }
            if (hasTerrainGrass && !hasTerrain) {
                invalidScene("TerrainGrassComponent requires TerrainComponent");
            }
        }

        expect(input, "END_SCENE");
        std::string trailing;
        if (input >> trailing) {
            invalidScene("unexpected data after END_SCENE");
        }

        std::unordered_map<UUID, Entity> entitiesByUuid;
        std::unordered_map<UUID, UUID> parentsByUuid;
        loaded.view<UUIDComponent>([&](const Entity entity, const UUIDComponent &uuid) {
            if (!entitiesByUuid.emplace(uuid.value, entity).second) {
                invalidScene("object UUID is duplicated");
            }
            if (loaded.has<ParentComponent>(entity)) {
                const UUID parent = loaded.get<ParentComponent>(entity).parent;
                if (parent == uuid.value) {
                    invalidScene("object cannot be its own parent");
                }
                parentsByUuid.emplace(uuid.value, parent);
            }
        });
        for (const auto &[child, parent]: parentsByUuid) {
            if (!entitiesByUuid.contains(parent)) {
                invalidScene("parent UUID does not refer to an object in this scene");
            }
            std::unordered_set<UUID> visited;
            UUID current = child;
            auto parentIt = parentsByUuid.find(current);
            while (parentIt != parentsByUuid.end()) {
                current = parentIt->second;
                if (!visited.insert(current).second) { invalidScene("parent hierarchy contains a cycle"); }
                parentIt = parentsByUuid.find(current);
            }
        }

        loaded.view<ColliderComponent, MeshRenderer>([](const Entity,
                                                         ColliderComponent& collider,
                                                         const MeshRenderer& renderer) {
            if (auto* meshCollider = std::get_if<MeshCollider>(&collider.shape)) {
                meshCollider->mesh = renderer.mesh;
            }
        });
        registry = std::move(loaded);
    }
} // namespace Engine

// NOLINTEND(readability-magic-numbers)
