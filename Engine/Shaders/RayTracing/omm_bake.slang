// Bakes 2-state opacity micromaps (VK_EXT_opacity_micromap) for alpha-tested
// geometry, so it can join the ray query TLAS without any-hit shading: each
// triangle is split into 4^level micro-triangles and every micro-triangle is
// marked opaque or transparent from the material's alpha at its centre, with
// the same cutout as Shadow/shadow_map.slang. Traversal then discards the
// transparent ones in hardware.
//
// One invocation fills one 32-bit word (32 micro-triangles) of one triangle;
// the first word of a triangle also writes its VkMicromapTriangleEXT and its
// entry of the BLAS's micromap index buffer (identity, so the BLAS usage
// counts equal the triangle count).
//
// Set 0 is ShadowPass's scene layout: materials (2) and their textures (13).
import Common.SceneData;
import Common.Displacement; // materialTextures

[[vk::binding(2, 0)]] StructuredBuffer<MaterialData> materials;

struct BakeConstants
{
    uint* vertices;   // GpuVertex heap, 8 words per vertex
    uint* indices;    // the section's first index (absolute vertex indices)
    uint* data;       // wordsPerTriangle words per triangle
    uint* triangles;  // VkMicromapTriangleEXT: dataOffset, level | format << 16
    uint* ommIndices; // one micromap index per triangle
    uint triangleCount;
    uint materialBase; // per-instance material table offset (RendererInstanceData)
    uint level;        // subdivision level, at least 3 (64 micro-triangles)
    uint padding;
};
[[vk::push_constant]] ConstantBuffer<BakeConstants> bake;

static const uint VertexWords = 8u;  // sizeof(GpuVertex) / 4
static const uint TexCoordWord = 4u; // two half floats
static const uint TexCoord1Word = 5u;
static const uint MaterialWord = 7u;
static const uint Format2State = 1u; // VK_OPACITY_MICROMAP_FORMAT_2_STATE_EXT

// Compacts the even bits of x into its low half.
uint compactEvenBits(uint x)
{
    x &= 0x55555555u;
    x = (x | (x >> 1u)) & 0x33333333u;
    x = (x | (x >> 2u)) & 0x0f0f0f0fu;
    x = (x | (x >> 4u)) & 0x00ff00ffu;
    x = (x | (x >> 8u)) & 0x0000ffffu;
    return x;
}

// Each bit becomes the XOR of itself and every higher bit.
uint suffixXor(uint x)
{
    x ^= x >> 1u;
    x ^= x >> 2u;
    x ^= x >> 4u;
    x ^= x >> 8u;
    return x;
}

// Barycentric (weights of vertices 1 and 2) centre of micro-triangle `index`
// in the space-filling ("bird") order the Vulkan specification defines for
// micromaps: the index's bit pairs walk the four sub-triangles of every
// level, the middle and the third one mirrored.
float2 microTriangleCentre(uint index, uint level)
{
    const uint evenBits = compactEvenBits(index);
    const uint oddBits = compactEvenBits(index >> 1u);
    const uint flipX = suffixXor(evenBits);
    const uint flipY = suffixXor(evenBits & ~oddBits);
    const uint turn = flipY ^ oddBits;
    const uint mask = (1u << level) - 1u;
    uint u = ((flipX & ~turn) | (evenBits & ~turn) | (~evenBits & ~flipX & turn)) & mask;
    uint v = (flipY ^ evenBits) & mask;
    const uint w = ((~flipX & ~turn) | (evenBits & ~turn) | (~evenBits & flipX & turn)) & mask;
    // Upright micro-triangles have their right angle at (u, v); the others
    // at (u + 1, v + 1), pointing back.
    const bool upright = ((u ^ v ^ w) & 1u) != 0u;
    const float step = 1.0 / float(1u << level);
    const float2 corner = (float2(float(u), float(v)) + (upright ? 0.0 : 1.0)) * step;
    return corner + (upright ? step : -step) / 3.0;
}

float2 unpackHalf2(uint packed)
{
    return float2(f16tof32(packed & 0xffffu), f16tof32(packed >> 16u));
}

// Material alpha at a UV pair, at the mip whose texels match `texelArea`
// (in level-0 texels) of one micro-triangle; Shadow/shadow_map.slang decides
// with the same textures, channels and cutoff.
bool opaqueAt(MaterialData material, float2 uv0, float2 uv1, float uvArea)
{
    const uint alphaMode = uint((material.textureIndices.w >> 1) & 3);
    if (alphaMode != 1u && alphaMode != 2u)
        return true;
    float alpha = material.roughnessAmbientOcclusion.w;
    if (material.textureIndices.x >= 0)
    {
        const Sampler2D texture = materialTextures[NonUniformResourceIndex(material.textureIndices.x)];
        uint width, height;
        texture.GetDimensions(width, height);
        const float lod = 0.5 * log2(max(uvArea * float(width) * float(height), 1.0));
        alpha *= texture.SampleLevel(materialTextureUv(material, 0, uv0, uv1), lod).a;
    }
    if (material.auxiliaryTextureIndices.y >= 0)
    {
        const Sampler2D texture = materialTextures[NonUniformResourceIndex(material.auxiliaryTextureIndices.y)];
        uint width, height;
        texture.GetDimensions(width, height);
        const float lod = 0.5 * log2(max(uvArea * float(width) * float(height), 1.0));
        alpha *= texture.SampleLevel(materialTextureUv(material, 4, uv0, uv1), lod).r;
    }
    const float cutoff = alphaMode == 1u ? material.roughnessAmbientOcclusion.z : 0.30;
    return alpha >= cutoff;
}

[numthreads(64, 1, 1)]
[shader("compute")]
void main(uint3 id : SV_DispatchThreadID)
{
    const uint microTriangles = 1u << (2u * bake.level);
    const uint wordsPerTriangle = microTriangles / 32u;
    const uint triangle = id.x / wordsPerTriangle;
    const uint word = id.x % wordsPerTriangle;
    if (triangle >= bake.triangleCount)
        return;

    float2 uv0[3];
    float2 uv1[3];
    uint materialIndex = 0u;
    [unroll] for (uint corner = 0u; corner < 3u; ++corner)
    {
        const uint vertex = bake.indices[triangle * 3u + corner];
        uv0[corner] = unpackHalf2(bake.vertices[vertex * VertexWords + TexCoordWord]);
        uv1[corner] = unpackHalf2(bake.vertices[vertex * VertexWords + TexCoord1Word]);
        if (corner == 0u)
            materialIndex = bake.vertices[vertex * VertexWords + MaterialWord];
    }
    const MaterialData material = materials[bake.materialBase + materialIndex];
    // UV-space area of one micro-triangle, for the sampled mip.
    const float2 edge1 = uv0[1] - uv0[0];
    const float2 edge2 = uv0[2] - uv0[0];
    const float microArea = 0.5 * abs(edge1.x * edge2.y - edge1.y * edge2.x) / float(microTriangles);

    uint bits = 0u;
    for (uint bit = 0u; bit < 32u; ++bit)
    {
        const float2 b = microTriangleCentre(word * 32u + bit, bake.level);
        const float3 weights = float3(1.0 - b.x - b.y, b.x, b.y);
        const float2 uv = uv0[0] * weights.x + uv0[1] * weights.y + uv0[2] * weights.z;
        const float2 uvSecond = uv1[0] * weights.x + uv1[1] * weights.y + uv1[2] * weights.z;
        if (opaqueAt(material, uv, uvSecond, microArea))
            bits |= 1u << bit;
    }
    bake.data[triangle * wordsPerTriangle + word] = bits;
    if (word == 0u)
    {
        bake.triangles[triangle * 2u] = triangle * wordsPerTriangle * 4u;
        bake.triangles[triangle * 2u + 1u] = bake.level | (Format2State << 16u);
        bake.ommIndices[triangle] = triangle;
    }
}
