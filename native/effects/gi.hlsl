// AnyAPI bounce light: one bounce of screen-space indirect light. Lit surfaces
// near a pixel tint it with their colour (a red wall warms the floor beside
// it), added before volumetric lighting. Only what is on screen can bounce.
cbuffer Frame : register(b0) {
    float4 metrics;     // 1/width, 1/height, width, height
    float4 right_axis;  // w: tangent of half horizontal field of view
    float4 up_axis;     // w: tangent of half vertical field of view
    float4 forward_axis; // w: maximum distance
    float4 projection;  // A B C D: depth = (A*z + B) / (C*z + D)
    float4 params;      // radius (m), strength, frame index, fade distance (m)
};
Texture2D colour : register(t0);  // copy of the HDR scene
Texture2D depth : register(t1);
Texture2D normals : register(t2); // gbuffer_1: world normal, w > 0.5 = clouds
Texture2D albedo : register(t3);  // gbuffer_0
SamplerState linear_sampler : register(s0);
SamplerState point_sampler : register(s1);

struct Varying { float4 position : SV_Position; float2 uv : TEXCOORD0; };
Varying Vertex(uint id : SV_VertexID) {
    Varying v; v.uv = float2((id << 1) & 2, id & 2);
    v.position = float4(v.uv * float2(2, -2) + float2(-1, 1), 0, 1); return v;
}
float view_z(float d) {
    float denominator = d * projection.z - projection.x;
    float z = abs(denominator) > .000001 ? (projection.y - d * projection.w) / denominator : forward_axis.w;
    return clamp(abs(z), 0, forward_axis.w);
}
float3 position(float2 uv, out bool sky) {
    float d = depth.SampleLevel(point_sampler, uv, 0).r; sky = d <= 0;
    float2 ndc = uv * float2(2, -2) + float2(-1, 1);
    return (forward_axis.xyz + right_axis.xyz * ndc.x * right_axis.w + up_axis.xyz * ndc.y * up_axis.w) * view_z(d);
}
float noise(float2 p) { return frac(52.9829189 * frac(dot(p, float2(.06711056, .00583715)))); }

float4 Bounce(Varying v) : SV_Target {
    bool sky; float3 p = position(v.uv, sky);
    float4 n4 = normals.SampleLevel(point_sampler, v.uv, 0);
    float z = dot(p, forward_axis.xyz);
    if (sky || n4.w > .5 || params.y <= 0) return 0;
    float fade = 1 - smoothstep(params.w * .6, params.w, z); if (fade <= 0) return 0;
    float3 n = normalize(n4.xyz);
    float radius = params.x, pixels = min(radius * metrics.w * .5 / max(z * up_axis.w, .001), 200);
    if (pixels < 2) return 0;
    float angle = noise(v.position.xy + params.z * 5.588238) * 6.2831853, jitter = noise(v.position.yx * 1.7 + params.z * 3.1);
    float3 gathered = 0; const int count = 10;
    [loop] for (int i = 0; i < count; ++i) {
        float t = (i + jitter) / count, a = angle + i * 2.39996323;
        float2 uv = v.uv + float2(cos(a), sin(a)) * t * pixels * metrics.xy;
        if (any(uv < 0) || any(uv > 1)) continue;
        bool s; float3 q = position(uv, s); if (s) continue;
        float3 d = q - p; float dd = dot(d, d); if (dd < .0001) continue; float3 dir = d * rsqrt(dd);
        float3 nq = normalize(normals.SampleLevel(point_sampler, uv, 0).xyz);
        // The sender must face the receiver, and the receiver must face the sender.
        float w = saturate(dot(n, dir)) * saturate(dot(nq, -dir)) * saturate(1 - dd / (radius * radius * 4));
        gathered += min(colour.SampleLevel(point_sampler, uv, 0).rgb, 4) * w;
    }
    float3 a = albedo.SampleLevel(point_sampler, v.uv, 0).rgb;
    return float4(a * gathered * (2.0 / count) * params.y * fade, 0);
}
