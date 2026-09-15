[[vk::binding(0,0)]] Sampler2D waterMeta;
[[vk::binding(1,0)]] RWStructuredBuffer<uint> tileLists;
[[vk::binding(2,0)]] RWStructuredBuffer<uint> tierCounts;
struct Settings { uint width,height,tilesX,maxTiles; };
[[vk::binding(3,0)]] ConstantBuffer<Settings> settings;

groupshared uint groupTier;
groupshared uint groupHasWater;

[numthreads(8,8,1)]
[shader("compute")]
void main(uint3 gid:SV_GroupID,uint3 lid:SV_GroupThreadID,uint3 tid:SV_DispatchThreadID){
    if(lid.x==0&&lid.y==0){groupTier=0u;groupHasWater=0u;}
    GroupMemoryBarrierWithGroupSync();

    if(tid.x<settings.width&&tid.y<settings.height){
        float2 m=waterMeta.Load(int3(tid.xy,0)).xy;
        uint q=asuint(m.y);
        if((q&0x80000000u)!=0){
            uint reflection=q&3u;
            uint refraction=(q>>2u)&3u;
            uint tier=max(reflection,refraction);
            InterlockedMax(groupTier,tier);
            uint ignored=0; InterlockedOr(groupHasWater,1u,ignored);
        }
    }
    GroupMemoryBarrierWithGroupSync();

    if(lid.x==0&&lid.y==0&&groupHasWater!=0u){
        uint dst=0;
        InterlockedAdd(tierCounts[groupTier],1u,dst);
        if(dst<settings.maxTiles) tileLists[groupTier*settings.maxTiles+dst]=gid.y*settings.tilesX+gid.x;
    }
}
