#[compute]

#version 450

#VERSION_DEFINES

#include "rc_radiance_pack_inc.glsl"
#include "rc_trace_inc.glsl"
// rc_trace(origin, dir, aperture_tan, t_start, t_end) + set 2 backend

// Sparse-RC (cascaded, NON-SHARED) — TRACE pass. Dispatched ONCE PER CASCADE (PC.cascade),
// indirect-sized to bucket_cap_c * dirs_c. One thread per (slot, direction): slots are the
// stable probe identity, so we iterate the whole hash region and skip empty slots. A live
// slot traces this cascade's interval and writes into its OWN deterministic radiance region.

layout(local_size_x = 64) in;

layout(set = 0, binding = 0, std430) readonly buffer Buckets {
	uvec2 buckets[];
};
layout(set = 0, binding = 1, std430) readonly buffer Alloc {
	uint alloc_count[];
};
layout(set = 0, binding = 4, std430) readonly buffer LiveList {
	uint live_list[];
};
layout(set = 0, binding = 3, std430) readonly buffer ProbeData {
	vec4 probe_world[];
}; // xyz center, w cascade
layout(set = 0, binding = 6, std430) writeonly buffer ProbeRad {
	uint probe_radiance[];
}; // packed rgba; merge folds the continuation (top cascade: final)

struct CascadeDesc {
	float spacing;
	float t_start;
	float t_end;
	float aperture;
	uint dirs;
	uint oct_res;
	uint bucket_off;
	uint bucket_cap;
	uint probe_off;
	uint probe_cap;
	uint rad_off;
	uint _p0;
};
layout(set = 0, binding = 7, std430) readonly buffer Cascades {
	CascadeDesc cascades[];
};
layout(set = 0, binding = 5, std140) uniform CameraData {
	mat4 inv_proj;
	mat4 inv_view;
	mat4 fwd_proj;
	mat4 fwd_view;
	vec2 jitter;
	vec2 _pad;
}
cam;

layout(push_constant) uniform PC {
	uint cascade;
	uint local_trans;
	uint frame;
	uint amortize_n; // per-DIRECTION amortization
	uint probe_amortize; // per-PROBE amortization (bounds cone-march cost vs probe count)
	float frustum_margin; // NDC margin around the view frustum (probes outside are not re-traced)
}
pc;

const uint EMPTY = 0xffffffffu, INVALID = 0xffffffffu;

// FRUSTUM-LIMITED tracing: world seeding makes ~1M probes live, far too many to trace. A probe is only
// re-traced while it's inside the view frustum (expanded by a margin so rotation/edge-interpolation see
// already-fresh probes); off-screen probes keep their last-traced (cached) radiance. Placement stays
// view-independent (probes exist everywhere) — only the RADIANCE UPDATE is frustum-limited.
bool in_frustum(vec3 W) {
	vec4 c = cam.fwd_proj * (cam.fwd_view * vec4(W, 1.0));
	if (c.w <= 0.0) {
		return false; // behind the camera
	}
	vec3 ndc = c.xyz / c.w;
	float m = pc.frustum_margin;
	return abs(ndc.x) <= 1.0 + m && abs(ndc.y) <= 1.0 + m && ndc.z >= 0.0 && ndc.z <= 1.0;
}

vec3 oct_to_dir(vec2 e) {
	e = e * 2.0 - 1.0;
	vec3 v = vec3(e.x, e.y, 1.0 - abs(e.x) - abs(e.y));
	if (v.z < 0.0) {
		v.xy = (1.0 - abs(v.yx)) * sign(v.xy);
	}
	return normalize(v);
}

void main() {
	CascadeDesc cd = cascades[pc.cascade];
	// One thread per LIVE PROBE (loop dirs internally) — same dispatch shape as rc_patch_merge. The old
	// per-(probe,dir) dispatch launched ×dirs more workgroups (~262k at 1M probes), and that workgroup
	// COUNT alone (even when every thread early-outs) hangs the load; merge's per-probe count (~16k) is fine.
	uint i = gl_GlobalInvocationID.x; // i-th LIVE probe (compact)
	if (i >= alloc_count[pc.cascade]) {
		return; // past live list (dispatch rounding)
	}

	uint entry = live_list[cd.probe_off + i]; // compact → actual slot (+ bootstrap flag in bit 31)
	uint slot_local = entry & 0x7fffffffu;
	bool bootstrap = (entry & 0x80000000u) != 0u; // owner changed this frame → must refresh ALL dirs
	uint idx = cd.probe_off + slot_local;
	vec3 origin = probe_world[idx].xyz;

	// FRUSTUM-LIMITED (OPTIONAL, default OFF): only re-trace probes you can (nearly) see; off-screen probes
	// keep cached radiance. This is a cost bound for huge scenes, but it leaves never-faced probes BLACK
	// (seeded but never traced) until a re-seed catches them in view — visible as dark corridors behind the
	// camera. The cone-marches are cheap (the old "3 fps" was the per-pass debug syncs, not the marches), so
	// we trace EVERY in-window probe by default (frustum_margin < 0) → view-independent, no black areas.
	if (pc.frustum_margin >= 0.0 && !in_frustum(origin)) {
		return;
	}
	// Per-PROBE amortization within the frustum: a surviving probe re-traces only every probe_amortize-th
	// frame (round-robin by STABLE slot → trace/merge lockstep); NEW probes (bootstrap) always trace.
	if (!bootstrap && pc.probe_amortize > 1u && (slot_local % pc.probe_amortize) != (pc.frame % pc.probe_amortize)) {
		return; // this probe keeps ALL its directions' persisted radiance this frame
	}

	for (uint d = 0u; d < cd.dirs; ++d) {
		// Per-DIRECTION amortization: re-trace only the rotating 1/N subset of dirs; rest keep last frame's
		// value. Must match merge's frame/N for the lockstep.
		bool in_subset = (pc.amortize_n <= 1u) || ((d % pc.amortize_n) == (pc.frame % pc.amortize_n));
		if (!bootstrap && !in_subset) {
			continue; // keep this direction's persisted radiance
		}

		vec2 e = (vec2(float(d % cd.oct_res), float(d / cd.oct_res)) + 0.5) / float(cd.oct_res);
		vec3 dir = oct_to_dir(e);

		vec4 r = rc_trace(origin, dir, cd.aperture, cd.t_start, cd.t_end, pc.local_trans);

		// Write the RAW interval; merge folds the continuation in lockstep. Top cascade: this IS final.
		uint ridx = cd.rad_off + slot_local * cd.dirs + d;
		probe_radiance[ridx] = rc_pack_radiance(r);
	}
}
