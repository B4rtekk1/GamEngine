// One persistent atlas layer per pane. Dispatch only the new impact's bounds.
[[vk::binding(3, 1), vk::image_format("r32f")]] RWTexture2DArray<float> crackAtlas;
struct CrackStamp {
    float4 positionRadius; // xy local impact, z radius, w intensity
    float4 sizeWidth;      // xy pane size, z crack width
    uint4 tile;            // slot, seed, reset, resolution
    uint4 region;          // x, y, width, height
};
[[vk::push_constant]] ConstantBuffer<CrackStamp> stamp;

float hash(float n) { return frac(sin(n * 12.9898) * 43758.5453); }

[numthreads(8, 8, 1)]
[shader("compute")]
void main(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= stamp.region.zw)) return;
    uint2 pixel = id.xy + stamp.region.xy;
    float2 local = ((float2(pixel) + 0.5) / float(stamp.tile.w) - 0.5) * stamp.sizeWidth.xy;
    float2 delta = local - stamp.positionRadius.xy;
    float radius = length(delta);
    float phase = hash(float(stamp.tile.y % 65536)) * 6.2831853;
    float angle = atan2(delta.y, delta.x) + phase;
    float rays = 8.0 + floor(hash(float(stamp.tile.y % 65536) + 1) * 8.0);
    float angular = angle * rays / 6.2831853;
    float ray = floor(angular + 0.5);
    // Stable irregular ray lengths and narrow lateral waviness.
    float wave = sin(radius * 19.0 + ray * 3.1) * 0.045;
    float radialDistance = abs(frac(angular + 0.5 + wave) - 0.5) * 6.2831853 / rays * radius;
    float extent = stamp.positionRadius.z * lerp(0.62, 1.0, hash(ray + phase));
    float ringSpacing = max(stamp.positionRadius.z * 0.23, 0.015);
    float ringPosition = sqrt(radius / ringSpacing) + sin(angle * 3.0 + phase) * 0.08;
    float ringDistance = abs(frac(ringPosition + 0.5) - 0.5) *
        2.0 * sqrt(max(radius * ringSpacing, 0.000001));
    float pixelWidth = max(stamp.sizeWidth.x, stamp.sizeWidth.y) / float(stamp.tile.w);
    float width = max(stamp.sizeWidth.z, pixelWidth * 0.45);
    float crack = 1.0 - smoothstep(width * 0.25, width + pixelWidth * 0.5,
        min(radialDistance, ringDistance));
    crack *= (1.0 - smoothstep(extent * 0.8, extent, radius)) * stamp.positionRadius.w;
    uint3 location = uint3(pixel, stamp.tile.x);
    float previous = stamp.tile.z != 0 ? 0.0 : crackAtlas[location];
    crackAtlas[location] = max(previous, crack);
}
