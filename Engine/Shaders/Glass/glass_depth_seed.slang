// Seeds GlassPass's private depth with the opaque scene depth. The glass draws then reject
// panes behind opaque geometry in hardware (early depth), instead of
// launching the full glass shader only to discard the fragment.
[[vk::binding(2, 1)]] Sampler2D opaqueDepth;

struct SeedVertexOutput
{
    float4 position : SV_Position;
};

[shader("vertex")]
SeedVertexOutput vertexMain(uint vertexIndex : SV_VertexID)
{
    SeedVertexOutput output;
    const float2 position = float2((vertexIndex << 1) & 2, vertexIndex & 2);
    output.position = float4(position * 2.0 - 1.0, 0.0, 1.0);
    return output;
}

struct SeedFragmentOutput
{
    float depth : SV_Depth;
};

[shader("fragment")]
SeedFragmentOutput fragmentMain(SeedVertexOutput input)
{
    SeedFragmentOutput output;
    output.depth = opaqueDepth.Load(int3(int2(input.position.xy), 0)).r;
    return output;
}
