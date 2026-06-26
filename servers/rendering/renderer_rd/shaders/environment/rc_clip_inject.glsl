#[compute]

#version 450

#VERSION_DEFINES

#include "rc_light_eval_inc.glsl"

// Coarse clipmap inject, SLAB form. One thread per shell voxel: rgb = emission +
// albedo·sun·max(N·L,0)/π, a = occupancy. No SDF march (vis = 1) — far-field sun shadows
// are below coarse-voxel perceptibility; level 0 carries the sharp shadows.

layout(local_size_x = 4, local_size_y = 4, local_size_z = 4) in;

layout(set = 0, binding = 0, rgba16f) uniform image3D radiance; // in .a=occ, out rgb+occ
layout(set = 0, binding = 1, rgba8) uniform readonly image3D albedo_in;
layout(set = 0, binding = 2, rgba8) uniform readonly image3D normal_in;
layout(set = 0, binding = 3, rgba16f) uniform readonly image3D emission_in;
layout(set = 0, binding = 4, std430) readonly buffer Lights {
	RCLight lights[];
};

layout(push_constant) uniform PC {
	vec3 sun_dir;
	float blend_alpha;
	vec3 sun_color;
	float voxel_size; // was _p1
	ivec3 slab_lo;
	uint res;
	ivec3 slab_dim;
	uint light_count; // was _p2
	ivec3 phase;
	uint _p3;
}
pc;

ivec3 rel(ivec3 cell) {
	int R = int(pc.res);
	return ((cell - pc.phase) % R + R) % R;
}

// Coarse occupancy visibility, window-relative voxels, toward L up to reach_vox (≤32 steps).
// SOFT transmission, not a hard stop: each occupied coarse cell along the ray multiplies the
// remaining light by COARSE_TRANSMIT. A thin, gappy occluder (a canopy that's ~1 coarse cell
// thick -- which coarse voxelizes as solid because it can't resolve the gaps) then passes a chunk
// of sun, approximating the dappled average L0 resolves directly; a thick wall stacks down to ~0
// and stays shadowed. This is what keeps the floor lit across the L0->coarse handoff instead of
// cutting to black where the finer gaps stop being resolved.
const float COARSE_TRANSMIT = 0.5;
float clip_vis(vec3 rstart, vec3 L, float reach_vox) {
	float R = float(pc.res);
	vec3 rp = rstart + L * 1.5;
	int steps = int(min(reach_vox, 32.0));
	float vis = 1.0;
	for (int i = 0; i < steps; ++i) {
		vec3 r = rp + L * float(i);
		if (any(lessThan(r, vec3(0.0))) || any(greaterThanEqual(r, vec3(R)))) {
			return vis; // exited the window → the rest is open sky
		}
		ivec3 cell = ((ivec3(floor(r)) + pc.phase) % int(R) + int(R)) % int(R);
		if (imageLoad(radiance, cell).a > 0.5) {
			vis *= COARSE_TRANSMIT;
			if (vis < 0.04) {
				return 0.0; // enough stacked occluders → fully shadowed
			}
		}
	}
	return vis;
}

// "Is there a ceiling above me?" — clip_vis marches the level's OWN occupancy toward the sun, but at the
// coarse levels (L≥3, voxels 2–4 m) a 3–5 m roof is ~1 voxel and the march overshoots/under-resolves it,
// so a floor UNDER a roof past L0 gets baked with direct sun (the leak). Catch it directly: if any
// occupancy sits straight above this voxel within reach, it's roofed and cannot see the sun. Starts one
// voxel up (skip self), so a roof straddling into the cell above is caught even at the coarsest level.
bool roofed(vec3 rstart) {
	float R = float(pc.res);
	int steps = int(min(R, 32.0));
	for (int i = 1; i <= steps; ++i) {
		vec3 r = rstart + vec3(0.0, float(i), 0.0);
		if (r.y >= R) {
			return false; // open sky above
		}
		ivec3 cell = ((ivec3(floor(r)) + pc.phase) % int(R) + int(R)) % int(R);
		if (imageLoad(radiance, cell).a > 0.5) {
			return true; // ceiling → roofed
		}
	}
	return false;
}

void main() {
	ivec3 lid = ivec3(gl_GlobalInvocationID);
	if (any(greaterThanEqual(lid, pc.slab_dim))) {
		return;
	}
	ivec3 wv = pc.slab_lo + lid;
	ivec3 cell = ((wv % int(pc.res)) + int(pc.res)) % int(pc.res); // toroidal

	vec4 rad = imageLoad(radiance, cell);
	if (rad.a < 0.5) {
		return; // empty voxel
	}

	vec3 N = normalize(imageLoad(normal_in, cell).rgb * 2.0 - 1.0);
	vec3 alb = imageLoad(albedo_in, cell).rgb;
	vec3 em = imageLoad(emission_in, cell).rgb;

	float R = float(pc.res);
	vec3 rstart = vec3(rel(cell)) + 0.5;
	vec3 W = (vec3(wv) + 0.5) * pc.voxel_size;

	// Sky ambient: coarse voxels are far/big, so a sun-only coarse inject reads black wherever the
	// sun is occluded. Bake a flat skylight term (alb * sky) so distant shaded surfaces read as lit
	// ambient instead of pure black -- the long-range fill the coarse levels are FOR. (pc.sun_color
	// carries the sky colour now; the directional sun comes from the light buffer below.)
	vec3 Lo = em + alb * pc.sun_color;

	// Light every buffer light like level 0 does (rc_eval_light = alb.color.ndl/pi), so the coarse
	// sun matches L0 -- the directional comes from the light buffer, not a separate push constant.
	// Visibility is the coarse-occupancy march TOWARD EACH LIGHT (no SDF up here). This marches the
	// actual sun direction, so it shadows under solid roof but passes through the canopy gaps that
	// align with the (angled) sun -- the warm dappled sun the finer coarse levels can resolve. (We
	// don't use a straight-up roofed() test: that over-shadows an angled sun, blocking the gaps.)
	for (uint i = 0u; i < pc.light_count; ++i) {
		vec3 Ldir, radiance;
		float reach;
		if (!rc_eval_light(lights[i], W, N, alb, pc.voxel_size, R, Ldir, radiance, reach)) {
			continue;
		}
		Lo += radiance * clip_vis(rstart, Ldir, reach);
	}
	rad.rgb = mix(rad.rgb, Lo, pc.blend_alpha);
	imageStore(radiance, cell, rad);
}
