#[compute]

#version 450

#VERSION_DEFINES

layout(local_size_x = 4, local_size_y = 4, local_size_z = 4) in;

layout(set = 0, binding = 0, rgba16f) uniform readonly image3D seed_in; // HALF: xyz=nearest occ (window-rel, half), w=valid
layout(set = 0, binding = 1, rgba16f) uniform writeonly image3D seed_out; // HALF
layout(set = 0, binding = 2, rgba16f) uniform readonly image3D voxel_in; // FULL: .a = occupancy
layout(set = 0, binding = 3, r16f) uniform image3D sdf_out; // HALF: distance in FULL-res voxels (read+write: localized finalize mins with the prior field)

layout(push_constant) uniform PC {
	uint mode;
	int step;
	uint res; // HALF res (_vox_res/2)
	uint region; // 0 = whole grid (c = gid, toroidal wrap); 1 = localized window-relative slab
	ivec3 phase; // HALF phase (_vox_phase/2)
	uint _p0;
	ivec3 slab_lo; // window-relative HALF region origin (region==1)
	uint _p1;
	ivec3 slab_dim; // window-relative HALF region size (region==1)
	uint _p2;
}
pc;

const uint M_INIT = 0u, M_FLOOD = 1u, M_FINAL = 2u;

ivec3 rel(ivec3 c) {
	int R = int(pc.res);
	return ((c - pc.phase) % R + R) % R;
} // half-res texel → window-relative

ivec3 texel_of(ivec3 wr) {
	int R = int(pc.res);
	return ((wr + pc.phase) % R + R) % R;
} // window-relative → half-res texel (rel(texel_of(wr)) == wr for wr in [0,R))

void main() {
	int R = int(pc.res); // HALF res

	// Resolve this thread's storage texel `c` and its window-relative position `rc`. Whole-grid
	// (region 0) walks every texel directly with toroidal wrap -- byte-identical to before. The
	// localized path (region 1) walks only a window-relative slab [slab_lo, slab_lo+slab_dim):
	// the scrolled-in shell + a margin, so the per-scroll flood touches a thin band instead of
	// the whole field. The rest of sdf_out keeps its prior (valid, phase-independent) distances.
	ivec3 c, rc;
	if (pc.region == 1u) {
		ivec3 gid = ivec3(gl_GlobalInvocationID);
		if (any(greaterThanEqual(gid, pc.slab_dim))) {
			return;
		}
		rc = pc.slab_lo + gid; // window-relative (slab is clamped to [0,R) on dispatch)
		c = texel_of(rc);
	} else {
		c = ivec3(gl_GlobalInvocationID);
		if (any(greaterThanEqual(c, ivec3(R)))) {
			return;
		}
		rc = rel(c);
	}

	if (pc.mode == M_INIT) {
		// downsample full-res occupancy: half-voxel occupied if ANY of its 2x2x2 children is.
		// conservative (over-includes) → SDF underestimates clearance → safe (never over-skips).
		float occ = 0.0;
		ivec3 fc = c * 2;
		for (int z = 0; z < 2; ++z) {
			for (int y = 0; y < 2; ++y) {
				for (int x = 0; x < 2; ++x) {
					occ = max(occ, imageLoad(voxel_in, fc + ivec3(x, y, z)).a);
				}
			}
		}
		imageStore(seed_out, c, (occ > 0.5) ? vec4(vec3(rc), 1.0) : vec4(0.0));
		return;
	}
	if (pc.mode == M_FLOOD) {
		vec4 best = imageLoad(seed_in, c);
		float bestd = (best.w > 0.5) ? distance(vec3(rc), best.xyz) : 1e9;
		for (int z = -1; z <= 1; ++z) {
			for (int y = -1; y <= 1; ++y) {
				for (int x = -1; x <= 1; ++x) {
					if (x == 0 && y == 0 && z == 0) {
						continue;
					}
					ivec3 n;
					if (pc.region == 1u) {
						// Localized: neighbour in window-relative space; skip anything outside the
						// slab so the flood never reads a stale/out-of-band seed or wraps to the far
						// side of the window. Occluders beyond the margin are handled by the
						// finalize min() against the prior field, so this stays leak-safe.
						ivec3 wn = rc + ivec3(x, y, z) * pc.step;
						if (any(lessThan(wn, pc.slab_lo)) || any(greaterThanEqual(wn, pc.slab_lo + pc.slab_dim))) {
							continue;
						}
						n = texel_of(wn);
					} else {
						n = ((c + ivec3(x, y, z) * pc.step) % R + R) % R; // toroidal wrap
					}
					vec4 s = imageLoad(seed_in, n);
					if (s.w < 0.5) {
						continue;
					}
					float d = distance(vec3(rc), s.xyz); // window-relative, linear
					if (d < bestd) {
						bestd = d;
						best = s;
					}
				}
			}
		}
		imageStore(seed_out, c, best);
		return;
	}
	// M_FINAL — distance is in HALF-res voxels; ×2 → FULL-res voxel units so the trace's
	// `textureLod(rc_sdf, ...).r * voxel_size` stays correct with the full-res voxel_size.
	vec4 s = imageLoad(seed_in, c);
	float d = (s.w > 0.5) ? 2.0 * distance(vec3(rc), s.xyz) : float(R) * 2.0;
	if (pc.region == 1u) {
		// Localized floods see only occluders within the slab. The prior field already encodes
		// occluders outside it, so take the MIN: never let a localized reflood INCREASE a cell's
		// distance (that could skip over a real occluder = leak). Too-small is conservative/safe.
		d = min(d, imageLoad(sdf_out, c).r);
	}
	imageStore(sdf_out, c, vec4(d, 0.0, 0.0, 0.0));
}
