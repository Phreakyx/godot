#[compute]

#version 450

#VERSION_DEFINES

// Sparse-RC (cascaded, NON-SHARED) — REBUILD pass (dense-pool design). Dispatched ONCE PER CASCADE,
// one thread per dense id in [0, pcap). The hashmap was just cleared; this repopulates it from the
// PERSISTENT dense probe pool so probe identity is the stable dense id, never the (transient) slot:
//   • free id (last_seen == 0)            → skip.
//   • alive but OUT OF THE GRID WINDOW    → EVICT: push the id onto the free-list, mark it free.
//   • alive AND in-window                 → INSERT (key → global id) into the cleared hashmap AND
//                                           APPEND to the live list (so it traces this frame).
//
// VIEW-INDEPENDENCE (Build 1a): the live list — the set the trace/merge/gather iterate — is now built
// HERE from every alive in-window probe, NOT from the screen-driven ADD pass. So a probe keeps updating
// while it stays in range regardless of whether the camera is looking at it; off-screen probes no longer
// freeze. EVICTION is now SPATIAL (probe left the toroidal grid window), not age-based: an in-range probe
// is immortal (you need its lighting anyway), and only probes whose world cell scrolled out of the 64 m
// window — their voxel data overwritten — recycle to seed the new leading edge. ADD only allocates new
// ids + stamps last_seen; this pass owns liveness, bootstrap, and eviction.

layout(local_size_x = 64) in;

layout(set = 0, binding = 0, std430) coherent buffer Buckets {
	uvec2 buckets[];
};
layout(set = 0, binding = 1, std430) coherent buffer Alloc {
	uint alloc_count[];
}; // per-cascade live counter (reset by clear, bumped here)
layout(set = 0, binding = 2, std430) readonly buffer ProbeKeys {
	ivec4 probe_keys[];
};
layout(set = 0, binding = 3, std430) readonly buffer ProbeData {
	vec4 probe_world[];
}; // xyz center, w cascade
layout(set = 0, binding = 4, std430) writeonly buffer LiveList {
	uint live_list[];
};
layout(set = 0, binding = 8, std430) coherent buffer LastSeen {
	uint last_seen[];
}; // per dense id (0 = free)
layout(set = 0, binding = 9, std430) coherent buffer RadTag {
	uint rad_tag[];
}; // per dense id owner hash (bootstrap detect)
layout(set = 0, binding = 11, std430) coherent buffer FreeList {
	uint free_ids[];
}; // recycled id_local per [probe_off+i]
layout(set = 0, binding = 12, std430) coherent buffer AllocState {
	uint alloc_state[];
}; // [c*2]=free_top, [c*2+1]=next_id

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
	uint frame;
	uint cascade;
	uint _p0, _p1;
	vec4 win_min; // xyz = grid origin (world)
	vec4 win_max; // xyz = grid origin + extent (world)
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

void main() {
	CascadeDesc cd = cascades[pc.cascade];
	uint id_local = gl_GlobalInvocationID.x;
	if (id_local >= cd.probe_cap) {
		return;
	}
	uint gid = cd.probe_off + id_local;

	uint ls = last_seen[gid];
	if (ls == 0u) {
		return; // free id — nothing to do
	}

	// SPATIAL eviction: keep the probe while its center sits inside the grid window (expanded by a
	// margin so the trilinear gather's edge corners survive). Once the toroidal grid scrolls past it,
	// its voxel data is overwritten → it's stale → recycle the id to the free-list. In-range = immortal.
	vec3 c = probe_world[gid].xyz;
	float margin = 2.0 * cd.spacing;
	if (any(lessThan(c, pc.win_min.xyz - margin)) || any(greaterThan(c, pc.win_max.xyz + margin))) {
		uint t = atomicAdd(alloc_state[pc.cascade * 2u + 0u], 1u); // push (free_top++)
		free_ids[cd.probe_off + t] = id_local;
		last_seen[gid] = 0u; // now free (radiance kept; bootstraps on reuse)
		return;
	}

	// alive → insert (key → gid) into the freshly-cleared hashmap (linear probe, claim via CAS on .x)
	ivec4 key = probe_keys[gid];
	uint h = hash_ivec4(key);
	if (h >= 0xfffffffeu) {
		h = 1u; // same clamp as add/gather/merge
	}
	uint base = cd.bucket_off, cap = cd.bucket_cap;
	uint home = h % cap;
	for (uint p = 0u; p < MAX_LINEAR; ++p) {
		uint slot = base + ((home + p) % cap);
		if (atomicCompSwap(buckets[slot].x, EMPTY, h) == EMPTY) { // claimed a free slot for our hash
			buckets[slot].y = gid; // publish our dense id (sole writer of this slot)
			break;
		}
		// slot taken (same hash by another id, or a different hash) → keep probing for our own slot
	}

	// APPEND to the live list so the trace updates this probe this frame (view-independent: every
	// in-window probe traces, not just screen-visible ones). rad_tag mismatch (a freed/reused id, or a
	// brand-new id) bootstraps all dirs on the first trace → no stale radiance bleeds from a prior owner.
	uint boot = (rad_tag[gid] != h) ? 0x80000000u : 0u;
	rad_tag[gid] = h;
	uint li = atomicAdd(alloc_count[pc.cascade], 1u);
	live_list[cd.probe_off + li] = id_local | boot;
}
