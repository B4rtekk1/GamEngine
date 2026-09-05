import Common.SceneData;
import Common.GrassWind;
import Grass.GrassPackedData;

struct GrassPackedDeformation { uint packedCurrent; uint packedPrevious; };

float grassSnorm8(uint packed, uint shift)
{
    int value = int((packed >> shift) & 0xffu);
    value = value >= 128 ? value - 256 : value;
    return float(value) / 127.0;
}

float3 grassDeformedLocalPosition(float3 localPosition, float3 root,
                                  float yaw, float scale, GrassPackedDeformation deformation,
                                  RendererFrameData frame, bool previousFrame)
{
    uint packed = previousFrame ? deformation.packedPrevious : deformation.packedCurrent;
    float trampled = float((packed >> 16u) & 0xffu) / 255.0;
    float height = max(scale, 1.0e-4);
    float normalizedHeight = saturate(localPosition.y / height);
    float curve = normalizedHeight * normalizedHeight * (3.0 - 2.0 * normalizedHeight);
    float2 bendWorld = float2(grassSnorm8(packed, 0u), grassSnorm8(packed, 8u));
    float2 bendLocal = grassRotateYaw(float3(bendWorld.x, 0.0, bendWorld.y), -yaw).xz;
    float bendWeight = curve * trampled;
    localPosition.xz += bendLocal * bendWeight * height * 0.8;
    localPosition.y -= bendWeight * (0.25 + 0.30 * normalizedHeight) * height;
    float windTime = previousFrame ? frame.windGustFrequencyTime.w : frame.windGustFrequencyTime.z;
    // grassWindDisplacement works in the blade-local frame.  A yaw-only
    // inverse is sufficient for packed terrain grass.
    localPosition += grassWindDisplacement(root.xz, float4(0.0, -sin(yaw * 0.5), 0.0, cos(yaw * 0.5)),
                                           normalizedHeight, height, windTime,
                                           frame.windDirectionStrength, frame.windSourcePositionRange,
                                           frame.windGustFrequencyTime);
    return localPosition;
}

float3 grassPackedDeformedWorld(float3 vertexPosition, GrassPackedInstance instance,
                                GrassPackedCluster cluster, GrassPackedDeformation deformation,
                                RendererFrameData frame, bool previousFrame)
{
    float yaw = grassPackedYaw(instance);
    float scale = grassPackedScale(instance);
    float3 root = grassPackedWorldPosition(instance, cluster);
    return root + grassRotateYaw(grassDeformedLocalPosition(vertexPosition * scale, root, yaw,
                                                             scale, deformation, frame, previousFrame), yaw);
}
