// AnyAPI night sky: stars, a milky way and a moon on sky pixels, and moonlight
// on surfaces, added to the HDR scene after the game's sky, fog and ocean and
// before tone mapping. Blended additively, so with night 0 nothing changes.
cbuffer Frame : register(b0) {
    float4 metrics;   // 1/width, 1/height, width, height
    float4 right_axis; // w: tangent of half horizontal field of view
    float4 up_axis;    // w: tangent of half vertical field of view
    float4 forward_axis;
    float4 sun;        // toward the sun; w: night factor 0..1
    float4 moon;       // toward the moon; w: above-horizon fade
    float4 celestial;  // cos, sin of the sky rotation; seconds
    float4 params;     // stars, moon brightness, moonlight, milky way
};
Texture2D depth : register(t0);   // reversed: sky is 0
Texture2D normals : register(t1); // gbuffer_1: world normal
Texture2D albedo : register(t2);  // gbuffer_0
Texture2D ocean : register(t3);   // gbuffer_ocean: x flag, y distance (ocean writes no depth)
SamplerState linear_sampler : register(s0);
SamplerState point_sampler : register(s1);

struct Varying { float4 position : SV_Position; float2 uv : TEXCOORD0; };
Varying Vertex(uint id : SV_VertexID) {
    Varying v; v.uv = float2((id << 1) & 2, id & 2);
    v.position = float4(v.uv * float2(2, -2) + float2(-1, 1), 0, 1); return v;
}
float4 hash4(float3 p) {
    float4 q = frac(float4(p.xyzx) * float4(.1031, .1030, .0973, .1099));
    q += dot(q, q.wzxy + 33.33);
    return frac((q.xxyz + q.yzzw) * q.zywx);
}
float value_noise(float3 p) {
    float3 i = floor(p), f = frac(p); f = f * f * (3 - 2 * f);
    float a = hash4(i).x, b = hash4(i + float3(1, 0, 0)).x, c = hash4(i + float3(0, 1, 0)).x, d = hash4(i + float3(1, 1, 0)).x;
    float e = hash4(i + float3(0, 0, 1)).x, g = hash4(i + float3(1, 0, 1)).x, h = hash4(i + float3(0, 1, 1)).x, k = hash4(i + float3(1, 1, 1)).x;
    return lerp(lerp(lerp(a, b, f.x), lerp(c, d, f.x), f.y), lerp(lerp(e, g, f.x), lerp(h, k, f.x), f.y), f.z);
}
float3 stars(float3 d, float pixel) {
    float3 p = d * 150, cell = floor(p); float4 h = hash4(cell);
    if (h.w > .16) return 0;
    // Inset so a star never straddles its cell edge.
    float3 star = normalize(cell + .2 + .6 * h.xyz);
    float angle = length(d - star), radius = max(pixel * 1.1, .0004);
    float magnitude = pow(saturate(h.w / .16), 5);          // most stars faint, a few bright
    float brightness = lerp(2.5, .08, magnitude);
    float twinkle = .75 + .25 * sin(celestial.z * (1.5 + 3 * h.x) + h.y * 40);
    float3 tint = lerp(float3(1, .82, .62), float3(.72, .82, 1), h.z);
    return tint * brightness * twinkle * exp(-angle * angle / (radius * radius));
}
float3 milky_way(float3 d) {
    float band = exp(-pow(dot(d, normalize(float3(.3, .8, .5))) / .2, 2));
    float n = value_noise(d * 5) * .55 + value_noise(d * 11) * .3 + value_noise(d * 23) * .15;
    return float3(.55, .62, .85) * band * saturate(n * 1.6 - .35) * .05;
}
float3 moon_disc(float3 d, float pixel) {
    float3 m = normalize(moon.xyz); float3 offset = d - m; float angle = length(offset);
    const float radius = .022;
    float3 glow = float3(.6, .68, .85) * (exp(-angle * 28) * .06 + exp(-angle * 6) * .015);
    if (angle > radius + pixel * 2) return glow;
    float3 t = normalize(abs(m.y) > .99 ? float3(1, 0, 0) : cross(float3(0, 1, 0), m)), b = cross(m, t);
    float2 q = float2(dot(offset, t), dot(offset, b)) / radius; float r2 = dot(q, q);
    float3 n = normalize(q.x * t + q.y * b - sqrt(saturate(1 - r2)) * m);
    float lit = saturate(dot(n, normalize(sun.xyz)) * 1.2 + .1) + .03;   // phase, plus earthshine
    float maria = .72 + .28 * value_noise(float3(q * 3.5, 7.3)) - .12 * value_noise(float3(q * 9, 1.7));
    float edge = 1 - smoothstep(1 - pixel * 2 / radius, 1, sqrt(r2));
    return glow + float3(1, .97, .9) * 3 * lit * maria * edge;
}
float4 Sky(Varying v) : SV_Target {
    float night = sun.w; if (night <= 0) return 0;
    float2 ndc = v.uv * float2(2, -2) + float2(-1, 1);
    float3 d = normalize(forward_axis.xyz + right_axis.xyz * ndc.x * right_axis.w + up_axis.xyz * ndc.y * up_axis.w);
    float scene_depth = depth.SampleLevel(point_sampler, v.uv, 0).r;
    float2 sea = ocean.SampleLevel(point_sampler, v.uv, 0).xy;
    float3 colour = 0;
    if (scene_depth <= 0 && sea.x <= 0 && sea.y <= 0) {
        float pixel = 2 * up_axis.w * metrics.y;
        float3 c = float3(d.x, d.y * celestial.x + d.z * celestial.y, d.z * celestial.x - d.y * celestial.y);
        float horizon = smoothstep(-.02, .18, d.y);
        colour = (stars(c, pixel) * params.x + milky_way(c) * params.w) * horizon + moon_disc(d, pixel) * params.y * moon.w;
    } else if (scene_depth > 0) {
        // Moonlight: unshadowed (the game has no shadow map for it).
        float3 n = normalize(normals.SampleLevel(point_sampler, v.uv, 0).xyz);
        float3 a = albedo.SampleLevel(point_sampler, v.uv, 0).rgb;
        // About twice the game's night ambient at strength 1, so the moon casts a clear direction.
        colour = a * float3(.55, .65, .9) * .15 * params.z * moon.w * saturate(dot(n, normalize(moon.xyz)));
    }
    return float4(colour * night, 0);
}
