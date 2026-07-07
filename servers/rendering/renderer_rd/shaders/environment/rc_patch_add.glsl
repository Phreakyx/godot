#[compute]

#version 450

#VERSION_DEFINES

// Sparse-RC (cascaded, NON-SHARED) — ADD pass. DENSE-POOL find-or-allocate, WORLD-SEEDED.
//
// Dispatched over the voxel-grid SHELL that streamed in this frame (one thread per shell voxel), NOT
// over screen pixels. Occupied cells (voxel_occ.a) seed the 8 gather corners per cascade, so a probe
// exists for every in-range surface regardless of camera facing — the fix for view-dependent GI.
//
// The hashmap was cleared and the rebuild pass re-inserted every ALIVE probe (key → dense id). So a
// seeded cell is either FOUND here (a read → its stable dense id; nothing more to do — rebuild already
// kept it alive + in the live list) or ABSENT (new, or returned after eviction) → we ALLOCATE a fresh
// dense id (pop the free-list, else bump a counter), insert it, and seed_new() appends it to the live
// list with the bootstrap bit. Probe identity is the dense id, not the slot, so the map churns freely.
//
// Concurrent insert of one cell by its many corner-touchers: the SINGLE thread that wins the slot's .x
// CAS allocates the id and publishes .y; siblings that see it mid-insert (.y == INVALID) give up for the
// frame — the winner already seeded it, so there's no double-allocation and no duplicate.

layout(local_size_x = 4, local_size_y = 4, local_size_z = 4) in;

layout(set = 0, binding = 0, std430) coherent buffer Buckets {
	uvec2 buckets[];
};
layout(set = 0, binding = 1, std430) coherent buffer Alloc {
	uint alloc_count[];
}; // per-cascade live counter (bumped only when we append a NEW alloc)
layout(set = 0, binding = 2, std430) coherent buffer ProbeKeys {
	ivec4 probe_keys[];
};
layout(set = 0, binding = 3, std430) coherent buffer ProbeData {
	vec4 probe_world[];
}; // xyz center, w cascade
layout(set = 0, binding = 4, std430) writeonly buffer LiveList {
	uint live_list[];
};
layout(set = 0, binding = 8, std430) coherent buffer RadTag {
	uint rad_tag[];
}; // per dense id owner hash
// LIVE-LIST OWNERSHIP (Build 1a): REBUILD appends every alive CARRIED-OVER probe (alive at rebuild time);
// ADD appends only the probes it NEWLY allocates THIS frame (they don't exist at rebuild time → no
// overlap, no double-count). So a new probe still traces the SAME frame it's seeded (no 1-frame stale
// read at disocclusion edges), and every in-window probe traces regardless of camera facing.
layout(set = 0, binding = 10, std430) coherent buffer LastSeen {
	uint last_seen[];
}; // per dense id (0 = free)
layout(set = 0, binding = 11, std430) coherent buffer FreeList {
	uint free_ids[];
}; // recycled id_local
layout(set = 0, binding = 12, std430) coherent buffer AllocState {
	uint alloc_state[];
}; // [c*2]=free_top,[c*2+1]=next_id
// Static occupancy (dedicated r8 mirror written by the inject pass) — NOT voxel_tex (sampling voxel_tex
// here aliases the trace's set-2 sampler → device lost). Read as a STORAGE image (imageLoad), matching
// inject's storage write, so occ_tex stays in GENERAL layout throughout — no storage→sampler transition
// (the missing transition between inject and this pass on the full bake hung the early frames).
#ifdef RC_FAR_SEED
// FAR-SEED variant: occupancy comes from a COARSE clip level (rgba16f, .a = occupancy), sampled toroidally
// like the trace (fract(W/extent)). No normal grid — coarse voxels are large, so we seed at the cell centre
// (the half-voxel face offset is negligible at this scale and there's no coarse normal mirror).
layout(set = 0, binding = 6) uniform sampler3D coarse_occ;
#else
// Static occupancy (dedicated r8 mirror written by the inject pass) — NOT voxel_tex (sampling voxel_tex
// here aliases the trace's set-2 sampler → device lost). Read as a STORAGE image (imageLoad), matching
// inject's storage write, so occ_tex stays in GENERAL layout throughout — no storage→sampler transition
// (the missing transition between inject and this pass on the full bake hung the early frames).
layout(set = 0, binding = 6, r8) uniform readonly image3D occ_grid;
// Voxel face normal (same grid the inject reads). We seed from the surface FACE (voxel centre + n·½voxel),
// not the centre, so the seeded probe corners line up with where the screen-space GATHER samples (the face,
// from depth). Without this the corners are half a voxel behind the face → on sub-spacing walls every corner
// lands behind the surface and the gather's facing test drops them (the red holes we were papering over).
layout(set = 0, binding = 5, rgba8) uniform readonly image3D normal_grid;
#endif

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

layout(push_constant) uniform PC {
	ivec3 seed_lo; // absolute world voxel of the shell corner
	uint cascade_begin;
	ivec3 seed_dim; // shell size in voxels (dispatch extent)
	uint cascade_end;
	uint res; // vox_res (toroidal modulus)
	float voxel_size;
	uint frame;
	float coarse_extent; // RC_FAR_SEED: the far clip level's world extent (fract(W/extent) toroidal occ read)
}
pc;

const uint EMPTY = 0xffffffffu, INVALID = 0xffffffffu, MAX_LINEAR = 64u;

uint hash_ivec4(ivec4 k) {
	uint h = uint(k.x) * 73856093u ^ uint(k.y) * 19349663u ^ uint(k.z) * 83492791u ^ uint(k.w) * 2654435761u;
	h ^= h >> 15;
	h *= 2246822519u;
	h ^= h >> 13;
	return h;
}

// Allocate a fresh dense id for cascade c: pop the free-list (recycled), else bump the high-water
// counter. Returns a GLOBAL id (probe_off + local), or INVALID if the pool is full. Called only by the
// thread that won a slot's .x CAS — one allocation per new cell, so no per-pixel allocation storm.
uint alloc_id(uint c, CascadeDesc cd) {
	int t = int(atomicAdd(alloc_state[c * 2u + 0u], 0xffffffffu)); // free_top-- ; t = old value
	if (t > 0) {
		return cd.probe_off + free_ids[cd.probe_off + uint(t - 1)]; // valid pop
	}
	atomicAdd(alloc_state[c * 2u + 0u], 1u); // underflow → undo decrement
	uint b = atomicAdd(alloc_state[c * 2u + 1u], 1u); // bump next_id
	return (b < cd.probe_cap) ? (cd.probe_off + b) : INVALID;
}

// Seed a NEWLY ALLOCATED probe: mark it alive, set its owner hash, and append it to the live list with
// the bootstrap bit so it traces THIS frame (its radiance slot holds a prior owner's value / nothing →
// the first trace must overwrite, not blend). Called once per new cell, by the CAS-winning thread only,
// so there's no per-pixel atomic storm. Carried-over probes are appended by REBUILD, not here.
void seed_new(uint c, CascadeDesc cd, uint gid, uint h) {
	last_seen[gid] = pc.frame;
	rad_tag[gid] = h;
	uint li = atomicAdd(alloc_count[c], 1u);
	live_list[cd.probe_off + li] = (gid - cd.probe_off) | 0x80000000u; // id_local + bootstrap bit
}

void touch_cell(uint c, ivec3 cell, vec3 center) {
	CascadeDesc cd = cascades[c];
	ivec4 key = ivec4(cell, int(c));
	uint h = hash_ivec4(key);
	if (h >= 0xfffffffeu) {
		h = 1u; // same clamp as rebuild/gather/merge
	}
	uint base = cd.bucket_off, cap = cd.bucket_cap;
	uint home = h % cap;
	for (uint p = 0u; p < MAX_LINEAR; ++p) {
		uint slot = base + ((home + p) % cap);
		uint cur = buckets[slot].x;
		if (cur == EMPTY) { // chain end → cell ABSENT → insert
			uint prev = atomicCompSwap(buckets[slot].x, EMPTY, h);
			if (prev == EMPTY) { // I won → allocate a dense id + publish
				uint gid = alloc_id(c, cd);
				if (gid == INVALID) {
					return; // pool full → drop (slot gone next clear)
				}
				probe_keys[gid] = key;
				probe_world[gid] = vec4(center, float(c));
				memoryBarrierBuffer(); // key/world visible before .y is published
				buckets[slot].y = gid;
				seed_new(c, cd, gid, h); // NEW probe → append + bootstrap (traces this frame)
				return;
			}
			if (prev != h) {
				continue; // a different hash took it → keep probing
			}
			cur = h; // a same-hash sibling is inserting → handle below
		}
		if (cur == h) {
			uint gid = buckets[slot].y;
			if (gid == INVALID) {
				return; // sibling mid-insert; it seeds the cell → give up
			}
			if (probe_keys[gid] == key) {
				return; // carried-over probe: already alive + appended to the live list by REBUILD
			}
			// same hash, different key (rare full-32-bit collision) → keep probing
		}
		// occupied by a different hash → next slot
	}
	// chain full (load too high near this home) → cell skipped this frame
}

// Seed the 8 cells the GATHER will trilinear-read for a surface point at `world` (gather bases at
// floor(world/s - 0.5); match exactly so no neighbour is missing). The 8-corner splat avoids dead probes
// at grazing angles. touch_cell is idempotent (find-or-alloc), so seeding overlapping cells twice is cheap.
void seed_corners(uint c, float s, vec3 world) {
	vec3 sp = world / s - 0.5;
	ivec3 b = ivec3(floor(sp));
	for (int o = 0; o < 8; ++o) {
		ivec3 cl = b + ivec3(o & 1, (o >> 1) & 1, (o >> 2) & 1);
		touch_cell(c, cl, (vec3(cl) + 0.5) * s);
	}
}

void main() {
	// One thread per shell voxel (world seeding). The dispatch covers [seed_lo, seed_lo+seed_dim) in
	// ABSOLUTE world voxels; read occupancy at the toroidal cell and seed probes only where there's a
	// surface. This is view-independent: a probe exists for an occupied cell whenever it's in range,
	// no matter where the camera looks. (Replaces screen-pixel + depth seeding.)
	ivec3 off = ivec3(gl_GlobalInvocationID);
	if (any(greaterThanEqual(off, pc.seed_dim))) {
		return;
	}
	ivec3 wv = pc.seed_lo + off; // absolute world voxel (of this level's grid)
#ifdef RC_FAR_SEED
	// FAR: occupancy from the coarse clip level, sampled toroidally the same way the trace does
	// (fract(W/extent)). Seed at the voxel centre — no normal-face offset at coarse scale.
	vec3 wc = (vec3(wv) + 0.5) * pc.voxel_size;
	if (texture(coarse_occ, fract(wc / pc.coarse_extent)).a < 0.5) {
		return; // empty coarse voxel — no distant surface here, no far probe
	}
	for (uint c = pc.cascade_begin; c < pc.cascade_end; ++c) {
		seed_corners(c, cascades[c].spacing, wc);
	}
#else
	int R = int(pc.res);
	ivec3 cell = ((wv % R) + R) % R; // toroidal grid cell
	if (imageLoad(occ_grid, cell).r < 0.5) {
		return; // empty voxel — no surface here, no probe
	}
	// Seed BOTH the surface FACE and the voxel CENTRE. The screen gather reconstructs the world position
	// from depth: on a solid wall/floor that's the FACE, so the face-offset (voxel centre + n·½voxel) makes
	// the seeded corners line up with the gather's. But THIN / FOLIAGE geometry has no coherent voxel normal
	// (leaves face every way -> n averages wrong or ~0), so the depth surface sits ~at the voxel CENTRE and
	// the face-offset misaligns -> the gather finds no probe (the red canopy in the probe-occupancy view).
	// Seeding both covers both cases; touch_cell dedups the overlap. Centre is skipped when n~0 (offset==0).
	vec3 nrm = imageLoad(normal_grid, cell).rgb * 2.0 - 1.0;
	bool has_n = dot(nrm, nrm) > 0.0001;
	nrm = has_n ? normalize(nrm) : vec3(0.0);
	vec3 wc = (vec3(wv) + 0.5) * pc.voxel_size; // voxel centre (matches the gather on thin/foliage)
	vec3 wf = wc + nrm * (0.5 * pc.voxel_size); // surface face (matches the gather on solid walls/floors)
	for (uint c = pc.cascade_begin; c < pc.cascade_end; ++c) {
		float s = cascades[c].spacing;
		seed_corners(c, s, wf);
		if (has_n) {
			seed_corners(c, s, wc);
		}
	}
#endif
}
