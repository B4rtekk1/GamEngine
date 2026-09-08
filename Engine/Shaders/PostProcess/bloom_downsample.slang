struct V { float4 position : SV_Position; [[vk::location(0)]] float2 uv; };
[shader("vertex")] V vertexMain(uint i : SV_VertexID) { V o; float2 p=float2((i<<1)&2,i&2); o.uv=p; o.position=float4(p*2-1,0,1); return o; }
[[vk::binding(0,0)]] Sampler2D sourceImage;
struct Settings { float threshold, knee, inverseWidth, inverseHeight; }; [[vk::push_constant]] ConstantBuffer<Settings> settings;
[shader("fragment")] float4 fragmentMain(V input) : SV_Target { float2 t=float2(settings.inverseWidth,settings.inverseHeight); float3 c=0; c+=sourceImage.Sample(input.uv+t*float2(-1,-1)).rgb; c+=sourceImage.Sample(input.uv+t*float2(1,-1)).rgb; c+=sourceImage.Sample(input.uv+t*float2(-1,1)).rgb; c+=sourceImage.Sample(input.uv+t*float2(1,1)).rgb; c*=0.25; float b=max(c.r,max(c.g,c.b)); float s=clamp(b-settings.threshold+settings.knee,0,2*settings.knee); s=s*s/max(4*settings.knee,1e-4); return float4(c*max(b-settings.threshold,s)/max(b,1e-4),1); }
