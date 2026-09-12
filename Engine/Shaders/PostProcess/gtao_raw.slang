// Half-resolution, slice-based GTAO. Each slice finds two horizons and
// analytically integrates the projected normal over its visible arc.
struct VertexOutput { float4 position : SV_Position; [[vk::location(0)]] float2 uv; };
[shader("vertex")] VertexOutput vertexMain(uint i : SV_VertexID) { VertexOutput o; float2 p=float2((i<<1)&2,i&2); o.uv=p; o.position=float4(p*2-1,0,1); return o; }
[[vk::binding(0,0)]] Sampler2D depthTexture;
struct Settings { float4x4 inverseProjection; float fullWidth, fullHeight, radiusView, falloff; };
[[vk::push_constant]] ConstantBuffer<Settings> settings;
struct Input { [[vk::location(0)]] float2 uv; }; struct Output { [[vk::location(0)]] float4 value:SV_Target; };
static const float Pi = 3.14159265359;
float3 positionFromDepth(float2 uv,float z) { float4 p=mul(float4(uv*2-1,z,1),settings.inverseProjection); return p.xyz/max(p.w,1e-6); }
float3 normalFromDepth(float2 uv,float3 p) { float2 t=1/float2(settings.fullWidth,settings.fullHeight); float3 px=positionFromDepth(uv+float2(t.x,0),depthTexture.SampleLevel(uv+float2(t.x,0),0).r); float3 py=positionFromDepth(uv+float2(0,t.y),depthTexture.SampleLevel(uv+float2(0,t.y),0).r); return normalize(cross(px-p,py-p)); }
float integrateHorizon(float horizon, float normalAngle) { return (cos(normalAngle) + 2.0*horizon*sin(normalAngle) - cos(2.0*horizon-normalAngle))*0.25; }
[shader("fragment")] Output fragmentMain(Input input) {
    Output o; float z=depthTexture.SampleLevel(input.uv,0).r; if(z>=0.999999) { o.value=float4(1,1e6,0,0); return o; }
    float3 p=positionFromDepth(input.uv,z); float viewDepth=max(-p.z,1e-4); float3 n=normalFromDepth(input.uv,p);
    float2 projectionScale=float2(1.0/settings.inverseProjection[0][0],1.0/settings.inverseProjection[1][1]);
    float2 radiusUv=0.5*settings.radiusView*abs(projectionScale)/viewDepth;
    const uint directions=8, steps=4; float visibility=0;
    float noise=frac(sin(dot(floor(input.uv*float2(settings.fullWidth,settings.fullHeight)/2),float2(12.9898,78.233)))*43758.5453)*Pi;
    float3 viewDir=normalize(-p);
    [unroll] for(uint d=0;d<directions;++d) {
        float2 ray=float2(cos(noise+Pi*d/directions),sin(noise+Pi*d/directions));
        float3 screenTangent=normalize(float3(ray.x/projectionScale.x,ray.y/projectionScale.y,0));
        float3 bitangent=normalize(cross(viewDir,screenTangent)); float3 tangent=normalize(cross(bitangent,viewDir));
        float3 projectedNormal=n-bitangent*dot(n,bitangent); float normalLength=length(projectedNormal); if(normalLength<1e-4) continue;
        projectedNormal/=normalLength; float normalAngle=atan2(dot(projectedNormal,tangent),dot(projectedNormal,viewDir));
        float horizons[2]={Pi*.5,Pi*.5};
        [unroll] for(uint side=0;side<2;++side) [unroll] for(uint s=1;s<=steps;++s) {
            float t=(s-.5)/steps; float2 suv=clamp(input.uv+ray*(side==0?1.0:-1.0)*radiusUv*t,0,1); float sz=depthTexture.SampleLevel(suv,0).r; if(sz>=.999999) continue;
            float3 v=positionFromDepth(suv,sz)-p; float dist2=dot(v,v); if(dist2<1e-6) continue;
            float3 sliceV=v-bitangent*dot(v,bitangent); float sliceLength=length(sliceV); if(sliceLength<1e-5) continue;
            float horizon=acos(clamp(dot(sliceV/sliceLength,viewDir),0.0,1.0)); float attenuation=saturate(1.0-dist2/(settings.radiusView*settings.radiusView));
            horizons[side]=min(horizons[side],lerp(Pi*.5,horizon,attenuation));
        }
        visibility+=normalLength*(integrateHorizon(horizons[0],normalAngle)+integrateHorizon(horizons[1],-normalAngle));
    }
    o.value=float4(saturate(visibility/directions),viewDepth,0,1); return o;
}
