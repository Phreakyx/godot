#[compute]

#version 450

#VERSION_DEFINES

// Sparse-RC — TEMPORAL ACCUMULATION. The unified clipmap gather picks per-pixel between cascades by probe
// coverage; the cascade windows are camera-centred, so the near->far handoff sweeps across surfaces as the
// camera moves and the (sharp, correct) transition pops frame-to-frame. Blend each frame's half-res
// irradiance with the PREVIOUS frame's, reprojected by camera motion (the scene is static, so camera motion
// is the only reprojection needed), as an exponential moving average -> the handoff smooths over time and the
// GI denoises. History is ping-ponged: read prev, write cur (which the a-trous + upsample then consume).

layout(local_size_x = 8, local_size_y = 8) in;

layout(set = 0, binding = 0, rgba16f) uniform image2D irradiance; // current gather output (in) + blended (out)
layout(set = 0, binding = 1) uniform sampler2D history_read; // previous frame's accumulated irradiance
layout(set = 0, binding = 2, rgba16f) uniform writeonly image2D history_write; // this frame's accumulated (for next)
layout(set = 0, binding = 3, std140) uniform CameraData {
	mat4 inv_proj;
	mat4 inv_view;
	mat4 fwd_proj;
	mat4 fwd_view;
	vec2 jitter;
	vec2 _pad;
}
cam;

layout(set = 1, binding = 0) uniform sampler2D depth_input; // FULL-res depth (nearest)

layout(push_constant) uniform PC {
	mat4 prev_view_proj; // previous frame's fwd_proj * fwd_view (world -> prev clip)
	uint half_w, half_h;
	float z_near, z_far;
	float alpha; // history weight (0 = no accumulation / first frame, ~0.9 = strong smoothing)
	float _p0, _p1, _p2;
}
pc;

float lin(float raw) {
	return (raw < 0.00001) ? pc.z_far : pc.z_near / raw;
}
vec3 screen_to_world(vec2 sc, float l) {
	vec2 ndc = (sc + 0.5) / vec2(pc.half_w, pc.half_h) * 2.0 - 1.0;
	ndc.y = -ndc.y;
	vec4 vh = cam.inv_proj * vec4(ndc, 1.0, 1.0);
	vec3 vd = vh.xyz / vh.w;
	return (cam.inv_view * vec4(vd * (l / -vd.z), 1.0)).xyz;
}

void main() {
	ivec2 px = ivec2(gl_GlobalInvocationID.xy);
	if (px.x >= int(pc.half_w) || px.y >= int(pc.half_h)) {
		return;
	}
	vec4 cur = imageLoad(irradiance, px);

	// Background / no accumulation this frame: pass through and seed history with the current value.
	vec2 uv = (vec2(px) + 0.5) / vec2(pc.half_w, pc.half_h);
	float raw = texture(depth_input, uv).r;
	if (raw < 0.00001 || pc.alpha <= 0.0) {
		imageStore(history_write, px, cur);
		return;
	}

	// Reproject this pixel's world position into the previous frame and sample the accumulated history there.
	vec3 W = screen_to_world(vec2(px), lin(raw));
	vec4 clip = pc.prev_view_proj * vec4(W, 1.0);
	vec3 result = cur.rgb;
	if (clip.w > 0.0) {
		vec2 puv = (clip.xy / clip.w) * vec2(0.5, -0.5) + 0.5;
		if (all(greaterThanEqual(puv, vec2(0.0))) && all(lessThanEqual(puv, vec2(1.0)))) {
			vec3 hist = texture(history_read, puv).rgb;
			result = mix(cur.rgb, hist, pc.alpha); // EMA: alpha of history + (1-alpha) of current
		}
	}

	imageStore(irradiance, px, vec4(result, cur.a));
	imageStore(history_write, px, vec4(result, 1.0));
}
