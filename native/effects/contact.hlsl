// AnyAPI contact shadows: short screen-space rays toward the sun find the small
// shadows the game's shadow cascades are too coarse for (under parts, between
// blocks, where objects meet the ground), and remove that pixel's sunlight.
//
// The game lights a pixel as albedo * (sun * (N.L + rim) * shadow + ambient +
// back) * 0.4. The sunlight removed is rebuilt from the G-buffer and the
// game's own colours, and the result never drops below the pixel's ambient
// and back light, so pixels the game already shadows are left alone.
cbuffer Frame : register(b0) {
    float4 metrics;     // 1/width, 1/height, width, height
    float4 right_axis;  // w: tangent of half horizontal field of view
    float4 up_axis;     // w: tangent of half vertical field of view
    float4 forward_axis; // w: maximum distance
    float4 projection;  // A B C D: depth = (A*z + B) / (C*z + D)
    float4 sun;         // toward the sun; w: strength 0..1
    float4 sun_colour;  // the game's main light
    float4 sky_colour;  // the game's ambient (sky) light
    float4 back_colour; // the game's back light
    float4 params;      // ray length (m), thickness (m), frame index, fade distance (m)
};
Texture2D colour : register(t0);  // copy of the HDR scene
Texture2D depth : register(t1);
Texture2D normals : register(t2); // gbuffer_1: world normal, w > 0.5 = clouds
Texture2D albedo : register(t3);  // gbuffer_0: w = rim
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
float noise(float2 p) { return frac(52.9829189 * frac(dot(p, float2(.06711056, .00583715)))); }

float4 Contact(Varying v) : SV_Target {
    float4 scene = colour.SampleLevel(point_sampler, v.uv, 0);
    float d = depth.SampleLevel(point_sampler, v.uv, 0).r;
    float4 n4 = normals.SampleLevel(point_sampler, v.uv, 0);
    if (d <= 0 || n4.w > .5 || sun.w <= 0) return scene;
    float2 ndc = v.uv * float2(2, -2) + float2(-1, 1);
    float3 ray = forward_axis.xyz + right_axis.xyz * ndc.x * right_axis.w + up_axis.xyz * ndc.y * up_axis.w;
    float z = view_z(d); float3 p = ray * z;
    float fade = 1 - smoothstep(params.w * .6, params.w, z);
    float3 n = normalize(n4.xyz), l = normalize(sun.xyz);
    float facing = dot(n, l);
    float4 a = albedo.SampleLevel(point_sampler, v.uv, 0);
    // Surfaces turned away from the sun are already in the game's own shadow; a
    // ray from them toward the sun only pierces their own surface. Shadowing
    // them removed rim light the game never added and darkened whole interior
    // walls, leaving a bright band where the rays left the screen.
    float toward = saturate((facing - .05) / .15);
    float lit = saturate(facing) + a.w;
    if (fade <= 0 || toward <= 0) return scene;
    // Start off the surface so it cannot shadow itself, then march toward the sun.
    float3 start = p + n * (.02 + z * .001);
    float jitter = noise(v.position.xy + params.z * 5.588238);
    float occlusion = 0; const int steps = 12;
    [loop] for (int i = 0; i < steps; ++i) {
        float t = (i + jitter) / steps; float3 q = start + l * params.x * t * t;
        float qz = dot(q, forward_axis.xyz); if (qz <= .05) break;
        float2 uv = float2(dot(q, right_axis.xyz) / (qz * right_axis.w), dot(q, up_axis.xyz) / (qz * up_axis.w)) * float2(.5, -.5) + .5;
        if (any(uv < 0) || any(uv > 1)) break;
        float sd = depth.SampleLevel(point_sampler, uv, 0).r; if (sd <= 0) continue;
        float behind = qz - view_z(sd);
        if (behind > .005 + qz * .0005 && behind < params.y) { float2 edge = min(uv, 1 - uv); occlusion = saturate(min(edge.x, edge.y) * 20); break; }
    }
    occlusion *= fade * sun.w * toward;
    if (occlusion <= 0) return scene;
    float3 direct = a.rgb * sun_colour.rgb * lit * .4;
    float3 floor_light = a.rgb * (sky_colour.rgb * (.5 + .5 * n.y) + back_colour.rgb * max(dot(n.xz, l.xz), 0)) * .4;
    float3 shadowed = max(scene.rgb - direct * occlusion, min(scene.rgb, floor_light));
    return float4(shadowed, scene.a);
}
