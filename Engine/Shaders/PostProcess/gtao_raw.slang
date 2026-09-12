// Half-resolution GTAO horizon search. Output x is visibility; y stores view depth
// for edge-aware filtering and reconstruction in the following passes.
struct VertexOutput { float4 position : SV_Position; [[vk::location(0)]] float2 uv; };
[shader("vertex")] VertexOutput vertexMain(uint i : SV_VertexID) { VertexOutput o; float2 p=float2((i<<1)&2,i&2); o.uv=p; o.position=float4(p*2-1,0,1); return o; }
[[vk::binding(0,0)]] Sampler2D depthTexture;
struct Settings { float4x4 inverseProjection; float fullWidth, fullHeight, radius, falloff; };
[[vk::push_constant]] ConstantBuffer<Settings> settings;
struct Input { [[vk::location(0)]] float2 uv; }; struct Output { [[vk::location(0)]] float4 value:SV_Target; };
float3 positionFromDepth(float2 uv,float z) { float4 p=mul(float4(uv*2-1,z,1),settings.inverseProjection); return p.xyz/max(p.w,1e-6); }
float3 normalFromDepth(float2 uv,float3 p) { float2 t=1/float2(settings.fullWidth,settings.fullHeight); float3 px=positionFromDepth(uv+float2(t.x,0),depthTexture.SampleLevel(uv+float2(t.x,0),0).r); float3 py=positionFromDepth(uv+float2(0,t.y),depthTexture.SampleLevel(uv+float2(0,t.y),0).r); return normalize(cross(px-p,py-p)); }
[shader("fragment")] Output fragmentMain(Input input) {
    Output o; float z=depthTexture.SampleLevel(input.uv,0).r; if(z>=0.999999) { o.value=float4(1,1e6,0,0); return o; }
    float3 p=positionFromDepth(input.uv,z); float3 n=normalFromDepth(input.uv,p); float occ=0; const uint directions=8, steps=4;
    float noise=frac(sin(dot(floor(input.uv*float2(settings.fullWidth,settings.fullHeight)/2),float2(12.9898,78.233)))*43758.5453)*6.2831853;
    [unroll] for(uint d=0;d<directions;++d) { float2 ray=float2(cos(noise+6.2831853*d/directions),sin(noise+6.2831853*d/directions)); float horizon=-1; [unroll] for(uint s=1;s<=steps;++s) { float t=(s-.5)/steps; float2 suv=clamp(input.uv+ray*t*settings.radius/float2(settings.fullWidth,settings.fullHeight),0,1); float sz=depthTexture.SampleLevel(suv,0).r; if(sz>=.999999) continue; float3 v=positionFromDepth(suv,sz)-p; float dist2=dot(v,v); float elevation=dot(n,v)*rsqrt(max(dist2,1e-5)); horizon=max(horizon,elevation*saturate(1-dist2/(settings.falloff*settings.falloff))); } occ+=max(horizon,0); }
    o.value=float4(saturate(1-occ/directions),-p.z,0,1); return o;
}
