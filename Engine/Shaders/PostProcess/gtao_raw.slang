// Half-resolution, slice-based GTAO. Each slice finds two horizons and
// analytically integrates the projected normal over its visible arc.
struct VertexOutput { float4 position : SV_Position; [[vk::location(0)]] float2 uv; };
[shader("vertex")] VertexOutput vertexMain(uint i : SV_VertexID) { VertexOutput o; float2 p=float2((i<<1)&2,i&2); o.uv=p; o.position=float4(p*2-1,0,1); return o; }
[[vk::binding(0,0)]] Sampler2D linearDepthTexture;
struct Settings { float2 projectionScale; float fullWidth, fullHeight, radiusView, falloff; };
[[vk::push_constant]] ConstantBuffer<Settings> settings;
struct Input { [[vk::location(0)]] float2 uv; }; struct Output { [[vk::location(0)]] float value:SV_Target0; [[vk::location(1)]] float4 auxiliary:SV_Target1; };
static const float Pi = 3.14159265359;
// rawAo is R16G16B16A16_SFLOAT.  Keep the sky sentinel below FP16's largest
// finite value (65504), otherwise it is stored as infinity and INF-INF in
// the bilateral filter produces NaNs.
static const float BackgroundViewDepth = 65500.0;
// The perspective ray is interpolated algebraically from UV.  Because the
// prefilter stores positive view depth (-view.z), position reconstruction is
// just a multiply instead of an inverse-projection per sample.
float3 positionFromLinearDepth(float2 uv,float viewDepth) { return float3((uv*2-1)/settings.projectionScale,-1)*viewDepth; }
float3 normalFromDepth(float2 uv,float3 p) { float2 t=1/float2(settings.fullWidth,settings.fullHeight); float3 px=positionFromLinearDepth(uv+float2(t.x,0),linearDepthTexture.SampleLevel(uv+float2(t.x,0),0).r); float3 py=positionFromLinearDepth(uv+float2(0,t.y),linearDepthTexture.SampleLevel(uv+float2(0,t.y),0).r); return normalize(cross(px-p,py-p)); }
// The spatial denoiser consumes this directly.  Keeping the normal beside AO
// avoids reconstructing the same normal from depth in every filter tap.
float2 encodeOctNormal(float3 n) { n/=max(abs(n.x)+abs(n.y)+abs(n.z),1e-6); if(n.z<0) n.xy=(1-abs(n.yx))*float2(n.x>=0?1:-1,n.y>=0?1:-1); return n.xy*.5+.5; }
float integrateHorizon(float horizon, float normalAngle, float normalCos) { return (normalCos + 2.0*horizon*sin(normalAngle) - cos(2.0*horizon-normalAngle))*0.25; }
[shader("fragment")] Output fragmentMain(Input input) {
    Output o; float viewDepth=linearDepthTexture.SampleLevel(input.uv,0).r; if(viewDepth>=BackgroundViewDepth) { o.value=1; o.auxiliary=float4(BackgroundViewDepth,0,1,0); return o; }
    float3 p=positionFromLinearDepth(input.uv,viewDepth); float3 n=normalFromDepth(input.uv,p);
    float2 projectionScale=settings.projectionScale;
    float2 radiusUv=0.5*settings.radiusView*abs(projectionScale)/viewDepth;
    const uint directions=8, steps=4; float visibility=0;
    float noise=frac(sin(dot(floor(input.uv*float2(settings.fullWidth,settings.fullHeight)/2),float2(12.9898,78.233)))*43758.5453)*Pi;
    float3 viewDir=normalize(-p);
    [unroll] for(uint d=0;d<directions;++d) {
        float angle=noise+Pi*d/directions; float sinPhi,cosPhi; sincos(angle,sinPhi,cosPhi);
        float2 ray=float2(cosPhi,sinPhi);
        float3 screenTangent=normalize(float3(ray.x/projectionScale.x,ray.y/projectionScale.y,0));
        float3 bitangent=normalize(cross(viewDir,screenTangent)); float3 tangent=normalize(cross(bitangent,viewDir));
        float3 projectedNormal=n-bitangent*dot(n,bitangent); float normalLength=length(projectedNormal); if(normalLength<1e-4) continue;
        projectedNormal/=normalLength;
        float normalCos=saturate(dot(projectedNormal,viewDir));
        float normalAngle=sign(dot(tangent,projectedNormal))*acos(normalCos);
        // Start at the projected normal's unoccluded hemisphere, not at a
        // view-aligned pi/2 horizon. This avoids false AO at grazing angles.
        float lowHorizons[2]={cos(normalAngle+Pi*.5),cos(normalAngle-Pi*.5)};
        float horizons[2]={lowHorizons[0],lowHorizons[1]};
        [unroll] for(uint side=0;side<2;++side) [unroll] for(uint s=1;s<=steps;++s) {
            float t=(s-.5)/steps; float2 suv=clamp(input.uv+ray*(side==0?1.0:-1.0)*radiusUv*t,0,1);
            // Distant taps deliberately trade pixel precision for cache-friendly
            // depth.  The pyramid is an average, not the Hi-Z min/max chain.
            float mip=s < 3 ? 0 : s == 3 ? 1 : 2;
            float sampleDepth=linearDepthTexture.SampleLevel(suv,mip).r; if(sampleDepth>=BackgroundViewDepth) continue;
            float3 v=positionFromLinearDepth(suv,sampleDepth)-p; float dist2=dot(v,v); if(dist2<1e-6) continue;
            float3 sliceV=v-bitangent*dot(v,bitangent); float sliceLength2=dot(sliceV,sliceV); if(sliceLength2<1e-10) continue;
            float invSliceLength=rsqrt(sliceLength2); float horizonCos=dot(sliceV,viewDir)*invSliceLength; float attenuation=saturate(1.0-dist2/(settings.radiusView*settings.radiusView));
            horizons[side]=max(horizons[side],lerp(lowHorizons[side],horizonCos,attenuation));
        }
        float h0=-acos(clamp(horizons[1],-1.0,1.0));
        float h1= acos(clamp(horizons[0],-1.0,1.0));
        h0=normalAngle+clamp(h0-normalAngle,-Pi*.5,Pi*.5);
        h1=normalAngle+clamp(h1-normalAngle,-Pi*.5,Pi*.5);
        visibility+=normalLength*(integrateHorizon(h0,normalAngle,normalCos)+integrateHorizon(h1,normalAngle,normalCos));
    }
    o.value=saturate(visibility/directions);
    o.auxiliary=float4(viewDepth,encodeOctNormal(n),0);
    return o;
}
