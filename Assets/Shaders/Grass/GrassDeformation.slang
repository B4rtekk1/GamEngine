import Common.SceneData;
import Common.GrassWind;
import Grass.GrassPackedData;

struct GrassPackedDeformation { uint packedCurrent; uint packedPrevious; };
struct GrassDeformedVertex { float3 position; float3 normal; };

float grassSnorm8(uint packed, uint shift)
{
    int value = int((packed >> shift) & 0xffu);
    value = value >= 128 ? value - 256 : value;
    return float(value) / 127.0;
}

float3 grassRotateAxisAngle(float3 value, float3 axis, float angle)
{
    float s, c;
    sincos(angle, s, c);
    return value * c + cross(axis, value) * s + axis * dot(axis, value) * (1.0 - c);
}

GrassDeformedVertex grassPackedDeformedVertex(float3 vertexPosition, float3 vertexNormal,
                                              GrassPackedInstance instance, GrassPackedCluster cluster,
                                              GrassPackedDeformation deformation,
                                              RendererFrameData frame, bool previousFrame)
{
    float yaw = grassPackedYaw(instance);
    float yawSine, yawCosine;
    sincos(yaw, yawSine, yawCosine);
    float scale = grassPackedScale(instance);
    float3 root = grassPackedWorldPosition(instance, cluster);
    float3 localPosition = vertexPosition * scale;
    float3 localNormal = vertexNormal;
    uint packed = previousFrame ? deformation.packedPrevious : deformation.packedCurrent;
    float trampled = float((packed >> 16u) & 0xffu) / 255.0;
    float baseY = cluster.bladeShape.x * scale;
    float height = max(cluster.bladeShape.y * scale, 1.0e-4);
    float normalizedHeight = saturate((localPosition.y - baseY) / height);
    float2 bendWorld = float2(grassSnorm8(packed, 0u), grassSnorm8(packed, 8u));
    float windTime = previousFrame ? frame.windGustFrequencyTime.w : frame.windGustFrequencyTime.z;
    GrassWindSample wind = grassWindSample(root.xz, grassPackedSeed(instance), yawSine, yawCosine, windTime,
                                           frame.windDirectionStrength, frame.windSourcePositionRange,
                                           frame.windGustFrequencyTime);
    float2 interactionLocal = grassRotateYawSinCos(float3(bendWorld.x, 0.0, bendWorld.y), -yawSine, yawCosine).xz;
    float2 targetDirection = wind.localDirection * wind.amount + interactionLocal * trampled;
    float targetLength = length(targetDirection);
    if (targetLength > 1.0e-5) {
        float stiffnessVariation = lerp(0.75, 1.25, grassWindHash(float2(grassPackedSeed(instance), 19.17)));
        float response = targetLength / max(cluster.bladeShape.z * stiffnessVariation, 1.0e-4);
        float angle = cluster.bladeShape.w * (1.0 - exp(-response)) * normalizedHeight * normalizedHeight;
        float2 direction = targetDirection / targetLength;
        float3 bendAxis = normalize(float3(direction.y, 0.0, -direction.x));
        float3 pivot = float3(0.0, baseY, 0.0);
        localPosition = pivot + grassRotateAxisAngle(localPosition - pivot, bendAxis, angle);
        localNormal = grassRotateAxisAngle(localNormal, bendAxis, angle);
    }
    GrassDeformedVertex result;
    result.position = root + grassRotateYawSinCos(localPosition, yawSine, yawCosine);
    result.normal = grassRotateYawSinCos(localNormal, yawSine, yawCosine);
    return result;
}
