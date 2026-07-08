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
// Level 0's lit radiance grid (mipped). Where a coarse cell overlaps L0 we take L0's already-
// correct radiance (downsampled to this level's mip) instead of re-lighting coarsely -- L0
// resolves the fine canopy gaps this level can't, so this keeps the L0->coarse handoff seamless.
layout(set = 0, binding = 5) uniform sampler3D l0_radiance;
struct LevelDesc {
	vec3 origin;
	float voxel_size;
	vec3 extent;
	float pad;
};
layout(set = 0, binding = 6, std140) uniform Clip {
	LevelDesc lvl[5]; // [0] = level 0 (origin/extent of the fine grid)
	uint num_levels;
	uint _cp0, _cp1, _cp2;
}
clip;

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
	uint level; // this coarse level (1..4); L0 overlap downsamples l0_radiance at mip = level
}
pc;

ivec3 rel(ivec3 cell) {
	int R = int(pc.res);
	return ((cell - pc.phase) % R + R) % R;
}

// Coarse occupancy visibility, window-relative voxels, toward L up to reach_vox (≤32 steps).
// HARD shadow: the first occupied coarse cell along the ray fully blocks the sun (vis = 0). Coarse
// voxelizes the gappy canopy as solid (it can't resolve the gaps), so soft transmission over-lit the
// far field -- a binary occupancy test keeps distant shadows crisp instead. (The L0 overlap region
// still uses L0's fine dappled radiance directly, so this only shapes the ring BEYOND the near grid.)
float clip_vis(vec3 rstart, vec3 L, float reach_vox) {
	float R = float(pc.res);
	vec3 rp = rstart + L * 1.5;
	int steps = int(min(reach_vox, 32.0));
	for (int i = 0; i < steps; ++i) {
		vec3 r = rp + L * float(i);
		if (any(lessThan(r, vec3(0.0))) || any(greaterThanEqual(r, vec3(R)))) {
			return 1.0; // exited the window → open sky, fully lit
		}
		ivec3 cell = ((ivec3(floor(r)) + pc.phase) % int(R) + int(R)) % int(R);
		if (imageLoad(radiance, cell).a > 0.5) {
			return 0.0; // occupied cell blocks the sun → fully shadowed
		}
	}
	return 1.0;
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

	// L0 overlap: where this coarse cell sits inside the fine grid, take L0's already-lit radiance
	// (downsampled to this level via mip = level) instead of re-lighting coarsely. L0 resolved the
	// fine canopy gaps this level can't, so the coarse level MATCHES it exactly here -> the L0->coarse
	// handoff is seamless. Only the ring BEYOND L0 falls through to the coarse sun/sky inject below.
	vec3 g0 = (W - clip.lvl[0].origin) / clip.lvl[0].extent;
	if (all(greaterThanEqual(g0, vec3(0.0))) && all(lessThan(g0, vec3(1.0)))) {
		vec3 l0 = textureLod(l0_radiance, fract(W / clip.lvl[0].extent.x), float(pc.level)).rgb;
		rad.rgb = mix(rad.rgb, l0, pc.blend_alpha);
		imageStore(radiance, cell, rad);
		return;
	}

	// Sky ambient: coarse voxels are far/big, so a sun-only coarse inject reads black wherever the
	// sun is occluded. Bake a skylight term (alb * sky), BUT gate it by SKY VISIBILITY -- otherwise every
	// enclosed coarse cell (a tunnel wall that can't see the sky) bakes flat skylight and the far field
	// reads as a bright sky-lit open space, incoherent with the shadowed, enclosed near field (the whole
	// point of far GI is to MATCH the near field at range). roofed() marches the coarse occupancy straight
	// up: a cell under a ceiling/canopy sees no sky (dark, like L0's enclosed cells), an open cell sees full
	// sky. This only shapes the ring BEYOND L0 (the overlap returned above using L0's own radiance).
	float sky_vis = roofed(rstart) ? 0.0 : 1.0;
	// NOTE: emission is NOT baked into the coarse radiance any more. It's stored persistently per level
	// (clip_emission[L]) and added directly by the trace (like L0), so distant emitters keep casting GI even
	// as this shell-local reflected radiance goes stale. Here we bake ONLY the reflected sun+sky.
	vec3 Lo = alb * pc.sun_color * sky_vis;

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
