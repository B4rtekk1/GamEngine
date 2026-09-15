// Reflection sources are resolved by confidence rather than added. Lower-
// confidence screen-space/RT results blend into stable probe/sky fallback,
// which avoids hard ownership changes and reflection popping at screen edges.
struct ReflectionCandidate
{
    float3 radiance;
    float confidence;
};

float3 resolveReflection(ReflectionCandidate rayTraced, ReflectionCandidate screenSpace,
                         ReflectionCandidate localProbe, ReflectionCandidate sky)
{
    float3 result = sky.radiance;
    result = lerp(result, localProbe.radiance, saturate(localProbe.confidence));
    result = lerp(result, screenSpace.radiance, saturate(screenSpace.confidence));
    result = lerp(result, rayTraced.radiance, saturate(rayTraced.confidence));
    return result;
}
