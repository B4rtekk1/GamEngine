// Reflection sources are resolved by confidence, never added together.  A
// screen-space or ray-traced hit owns the pixel only when its validation pass
// marks it reliable; otherwise the local capture and finally the sky remain a
// stable fallback for off-screen geometry.
struct ReflectionCandidate
{
    float3 radiance;
    float confidence;
};

float3 resolveReflection(ReflectionCandidate rayTraced, ReflectionCandidate screenSpace,
                         ReflectionCandidate localProbe, ReflectionCandidate sky)
{
    if (rayTraced.confidence > 0.0) return rayTraced.radiance;
    if (screenSpace.confidence > 0.0) return screenSpace.radiance;
    if (localProbe.confidence > 0.0) return localProbe.radiance;
    return sky.radiance;
}
