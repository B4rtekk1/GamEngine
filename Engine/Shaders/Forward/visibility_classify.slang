// Material classification of the visibility buffer (VisibilityBufferPass).
// Each 8x8 tile appends itself to the list of every material class among
// its pixels; one indirect dispatch per class then resolves only its tiles,
// so each class runs its own, lighter program instead of one uber shader
// whose register count every pixel pays.
import Common.MeshletDraw;

[[vk::binding(0, 0)]] Texture2D<uint> visibilityIds;
// Per class: `tileCapacity` packed tile coordinates (x | y << 16).
[[vk::binding(1, 0)]] RWStructuredBuffer<uint> classTiles;
// Per class: VkDispatchIndirectCommand padded to four words; x is the count.
[[vk::binding(2, 0)]] RWStructuredBuffer<uint> classDispatch;

struct ClassifyConstants
{
    uint tileCapacity;
};
[[vk::push_constant]] ConstantBuffer<ClassifyConstants> constants;

static const uint VisibilityTileSize = 8u;
groupshared uint tileClasses;

[numthreads(VisibilityTileSize, VisibilityTileSize, 1)]
[shader("compute")]
void main(uint3 groupId : SV_GroupID, uint3 id : SV_DispatchThreadID, uint groupIndex : SV_GroupIndex)
{
    if (groupIndex == 0u)
        tileClasses = 0u;
    GroupMemoryBarrierWithGroupSync();
    uint width, height;
    visibilityIds.GetDimensions(width, height);
    if (id.x < width && id.y < height)
    {
        const uint visibility = visibilityIds.Load(int3(id.xy, 0));
        const uint materialClass = visibilityClass(visibility);
        if (visibility != VisibilityNone && materialClass < VisibilityClassCount)
            InterlockedOr(tileClasses, 1u << materialClass);
    }
    GroupMemoryBarrierWithGroupSync();
    if (groupIndex < VisibilityClassCount && (tileClasses & (1u << groupIndex)) != 0u)
    {
        uint slot;
        InterlockedAdd(classDispatch[groupIndex * 4u], 1u, slot);
        classTiles[groupIndex * constants.tileCapacity + slot] = groupId.x | (groupId.y << 16u);
    }
}
