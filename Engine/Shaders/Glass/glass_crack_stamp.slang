// Rasterize CPU crack-graph edges and energy-limited surface polylines.
[[vk::binding(3, 1), vk::image_format("r32f")]] RWTexture2DArray<float> crackAtlas;
struct CrackStamp {
    float4 positionRadius; // xy segment start, zw segment end
    float4 sizeWidth;      // xy pane size, z crack width, w intensity
    uint4 tile;            // slot, reserved, reset, resolution
    uint4 region;
};
[[vk::push_constant]] ConstantBuffer<CrackStamp> stamp;
[numthreads(8, 8, 1)]
[shader("compute")]
void main(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= stamp.region.zw)) return;
    uint2 pixel = id.xy + stamp.region.xy;
    uint3 location = uint3(pixel, stamp.tile.x);
    if (stamp.tile.z != 0) { crackAtlas[location] = 0.0; return; }
    float2 local = ((float2(pixel) + 0.5) / float(stamp.tile.w) - 0.5) * stamp.sizeWidth.xy;
    float2 start = stamp.positionRadius.xy;
    float2 edge = stamp.positionRadius.zw - start;
    float t = saturate(dot(local - start, edge) / max(dot(edge, edge), 1e-12));
    float distance = length(local - start - edge * t);
    float pixelWidth = max(stamp.sizeWidth.x, stamp.sizeWidth.y) / float(stamp.tile.w);
    float width = stamp.sizeWidth.z;
    // Coverage preserves subtexel line energy instead of inflating thin cracks.
    float coverage = saturate((width * 0.5 + pixelWidth * 0.5 - distance) / pixelWidth);
    coverage = min(coverage, saturate(width / pixelWidth)) * stamp.sizeWidth.w;
    crackAtlas[location] = max(crackAtlas[location], coverage);
}
