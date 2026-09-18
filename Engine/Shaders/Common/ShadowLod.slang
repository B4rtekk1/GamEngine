// Shared geometric LOD policy for virtual shadow page marking and sampling.
// Keep this in one module so both passes make the same decision at the same
// footprint thresholds.

static const uint SHADOW_QUALITY_LOW = 0;
static const uint SHADOW_QUALITY_MEDIUM = 1;
static const uint SHADOW_QUALITY_HIGH = 2;
static const uint SHADOW_QUALITY_ULTRA = 3;
static const uint VIRTUAL_PAGES_PER_AXIS = 32;
static const uint PAGE_RESOLUTION = 256;
static const uint VIRTUAL_RESOLUTION = VIRTUAL_PAGES_PER_AXIS * PAGE_RESOLUTION;

float shadowTexelFootprintFromGradients(float2 uvDxLeft, float2 uvDxRight,
                                        float2 uvDyUp, float2 uvDyDown)
{
    return max(max(length(uvDxLeft), length(uvDxRight)),
               max(length(uvDyUp), length(uvDyDown))) * float(VIRTUAL_RESOLUTION);
}

float shadowTexelFootprintThreshold(const uint shadowQuality)
{
    if (shadowQuality == SHADOW_QUALITY_ULTRA) return 3.0;
    if (shadowQuality == SHADOW_QUALITY_HIGH) return 2.0;
    return shadowQuality == SHADOW_QUALITY_MEDIUM ? 1.5 : 1.0;
}
