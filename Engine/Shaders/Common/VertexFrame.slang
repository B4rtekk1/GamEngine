// Packed tangent frame of a GpuVertex (Engine/Renderer/Geometry/GpuVertex.h):
//   bits  0-10  normal, octahedral x, (q - 1023) / 1023
//   bits 11-21  normal, octahedral y
//   bits 22-30  tangent angle around the normal, q * 2pi / 512 - pi, measured
//               in the orthonormal basis buildVertexFrameBasis(normal)
//   bit  31     bitangent handedness (set: tangent.w = -1)
// Keep in sync with GpuVertex::packTangentFrame.
module VertexFrame;

public float3 decodeVertexNormal(uint frame)
{
    const float2 e = (float2(float(frame & 2047u), float((frame >> 11u) & 2047u)) - 1023.0) / 1023.0;
    float3 n = float3(e, 1.0 - abs(e.x) - abs(e.y));
    const float t = saturate(-n.z);
    n.x += n.x >= 0.0 ? -t : t;
    n.y += n.y >= 0.0 ? -t : t;
    return normalize(n);
}

// Duff et al. 2017, "Building an Orthonormal Basis, Revisited".
public void buildVertexFrameBasis(float3 n, out float3 b1, out float3 b2)
{
    const float s = n.z >= 0.0 ? 1.0 : -1.0;
    const float a = -1.0 / (s + n.z);
    const float b = n.x * n.y * a;
    b1 = float3(1.0 + s * n.x * n.x * a, s * b, -s * n.x);
    b2 = float3(b, s + n.y * n.y * a, -n.y);
}

public float4 decodeVertexTangent(uint frame, float3 normal)
{
    float3 b1, b2;
    buildVertexFrameBasis(normal, b1, b2);
    const float angle = float((frame >> 22u) & 511u) * (6.28318530718 / 512.0) - 3.14159265359;
    float s, c;
    sincos(angle, s, c);
    return float4(c * b1 + s * b2, (frame & 0x80000000u) != 0u ? -1.0 : 1.0);
}
