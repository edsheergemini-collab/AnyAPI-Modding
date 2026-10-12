// AnyAPI lens: depth of field and camera motion blur on the HDR scene, before
// the finishing pass. Depth of field focuses on what is in the middle of the
// screen. Motion blur follows the camera only (the game has no per-object
// motion), and nothing within a metre of the camera blurs (held items).
cbuffer Frame : register(b0) {
    float4 metrics;          // 1/width, 1/height, width, height
    float4 right_axis;       // w: tangent of half horizontal field of view
    float4 up_axis;          // w: tangent of half vertical field of view
    float4 forward_axis;     // w: maximum distance
    float4 projection;       // A B C D: depth = (A*z + B) / (C*z + D)
    float4 previous_right;   // last frame's camera; w 0 = no history
    float4 previous_up;
    float4 previous_forward;
    float4 camera_move;      // camera position now minus last frame (m); w: focus blend (1 = snap)
    float4 params;           // depth of field, motion blur, maximum blur (px), frame index
};
Texture2D colour : register(t0);  // copy of the HDR scene
Texture2D depth : register(t1);
Texture2D previous_focus : register(t2);  // 1x1 R32F: last frame's focus distance
Texture2D focus_distance : register(t3);  // 1x1 R32F: this frame's (written by Focus)
SamplerState linear_sampler : register(s0);
SamplerState point_sampler : register(s1);

struct Varying { float4 position : SV_Position; float2 uv : TEXCOORD0; };
Varying Vertex(uint id : SV_VertexID) {
    Varying v; v.uv = float2((id << 1) & 2, id & 2);
    v.position = float4(v.uv * float2(2, -2) + float2(-1, 1), 0, 1); return v;
}
float view_z(float d) {
    if (d <= 0) return forward_axis.w;
    float denominator = d * projection.z - projection.x;
    float z = abs(denominator) > .000001 ? (projection.y - d * projection.w) / denominator : forward_axis.w;
    return clamp(abs(z), 0, forward_axis.w);
}
float scene_z(float2 uv) { return view_z(depth.SampleLevel(point_sampler, uv, 0).r); }
float noise(float2 p) { return frac(52.9829189 * frac(dot(p, float2(.06711056, .00583715)))); }
// Blur radius in pixels for a pixel at distance z.
float circle(float z, float focus) { return min(params.x * abs(1 / max(focus, .5) - 1 / max(z, .5)) * metrics.w * .02, params.z); }

// Autofocus on the nearest of five taps around the screen centre, eased over
// time so the focus does not snap as the view sweeps past edges. Held items
// (closer than a metre) are ignored.
float Focus(Varying v) : SV_Target {
    float target = 300;
    [unroll] for (int k = 0; k < 5; ++k) {
        float2 o = k == 0 ? 0 : float2(k == 1 ? .03 : k == 2 ? -.03 : 0, k == 3 ? .04 : k == 4 ? -.04 : 0);
        float z = scene_z(.5 + o); if (z >= 1) target = min(target, z);
    }
    if (camera_move.w >= 1) return target;
    float last = previous_focus.Load(int3(0, 0, 0)).r;
    if (!(last >= 1 && last <= 300)) return target;
    // Ease in inverse distance, which is what the blur size follows.
    return 1 / lerp(1 / last, 1 / target, camera_move.w);
}

float4 Lens(Varying v) : SV_Target {
    float4 scene = colour.SampleLevel(point_sampler, v.uv, 0);
    float z = scene_z(v.uv);
    float focus = clamp(focus_distance.Load(int3(0, 0, 0)).r, 1, 300);
    float coc = params.x > 0 ? circle(z, focus) : 0;
    float2 velocity = 0;
    if (params.y > 0 && previous_right.w > 0 && z > 1) {
        float2 ndc = v.uv * float2(2, -2) + float2(-1, 1);
        float3 p = (forward_axis.xyz + right_axis.xyz * ndc.x * right_axis.w + up_axis.xyz * ndc.y * up_axis.w) * z + camera_move.xyz;
        float pz = dot(p, previous_forward.xyz);
        if (pz > .05) {
            float2 previous = float2(dot(p, previous_right.xyz) / (pz * previous_right.w), dot(p, previous_up.xyz) / (pz * previous_up.w)) * float2(.5, -.5) + .5;
            velocity = (v.uv - previous) * .5 * params.y;   // half-frame shutter
            float pixels = length(velocity * metrics.zw);
            if (pixels > params.z) velocity *= params.z / pixels;
            // Fade out close to the camera, where cockpits and vehicles ride along.
            velocity *= saturate((z - 1.5) / 4.5);
            if (pixels * saturate((z - 1.5) / 4.5) < .5) velocity = 0;
        }
    }
    if (coc < .5 && all(velocity == 0)) return scene;
    float jitter = noise(v.position.xy + params.w * 5.588238), angle = jitter * 6.2831853;
    float4 sum = float4(scene.rgb, 1); const int taps = 16;
    [loop] for (int i = 0; i < taps; ++i) {
        float t = (i + jitter) / taps, a = angle + i * 2.39996323;
        float2 offset = float2(cos(a), sin(a)) * sqrt(t) * coc * metrics.xy + velocity * (t - .5);
        float2 uv = clamp(v.uv + offset, metrics.xy * .5, 1 - metrics.xy * .5);
        float sz = scene_z(uv);
        // A sharp foreground sample must not smear over the pixel; held items never blur in.
        float w = sz < 1.5 ? 0 : sz + .5 >= z ? 1 : saturate(circle(sz, focus) / max(length(offset * metrics.zw), .5));
        sum += float4(colour.SampleLevel(linear_sampler, uv, 0).rgb, 1) * w;
    }
    return float4(sum.rgb / sum.w, scene.a);
}
