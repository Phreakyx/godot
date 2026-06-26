#[compute]

#version 450

#VERSION_DEFINES

// Unpack the SDFGI-style PASS_MODE_SDF voxelization output (packed integer grids) into
// Radiance Cascades' voxel grid, in the exact format the old CPU voxelizer produced:
//   voxel_out   rgba16f  radiance.rgb (0 here; inject fills it) + occupancy.a
//   albedo_out  rgba8    surface albedo.rgb
//   normal_out  rgba8    face normal * 0.5 + 0.5
//   emission_out rgba16f  emissive radiance.rgb
// One thread per render-grid cell. The render targets are in direct grid coords; RC's
// grid is toroidal, so we write cell (vox_phase + gp) % res (see rc_trace_inc fract()).

layout(local_size_x = 4, local_size_y = 4, local_size_z = 4) in;

// Packed inputs (SDFGI render_* formats; see scene_forward_clustered MODE_RENDER_SDF).
layout(set = 0, binding = 0, r16ui) uniform restrict readonly uimage3D src_albedo; // [solid:1|R:5|G:5|B:5]
layout(set = 0, binding = 1, r32ui) uniform restrict readonly uimage3D src_emission; // RGBE8985
layout(set = 0, binding = 2, r32ui) uniform restrict readonly uimage3D src_facing; // 6-bit axis mask

// RC voxel grid outputs.
layout(set = 0, binding = 3, rgba16f) uniform restrict writeonly image3D voxel_out;
layout(set = 0, binding = 4, rgba8) uniform restrict writeonly image3D albedo_out;
layout(set = 0, binding = 5, rgba8) uniform restrict writeonly image3D normal_out;
layout(set = 0, binding = 6, rgba16f) uniform restrict writeonly image3D emission_out;

layout(push_constant) uniform PC {
	ivec3 phase; // origin_voxel % res; toroidal write offset
	uint res;
	ivec3 slab_lo; // region origin in render-grid coords (0 for a full unpack)
	uint pad0;
	ivec3 slab_dim; // region size in voxels ((res,res,res) for a full unpack)
	uint pad1;
}
pc;

// Decode the RGBE8985 emission SDFGI packs (see scene_forward_clustered): sRed in bits
// 0-7 (<<1), sGreen in 8-16, sBlue in 17-24 (<<1), exponent in 25-29 (bias 15, mantissa 9).
vec3 decode_rgbe8985(uint v) {
	uint sr = (v & 0xFFu) << 1;
	uint sg = (v >> 8) & 0x1FFu;
	uint sb = ((v >> 17) & 0xFFu) << 1;
	uint e = (v >> 25) & 0x1Fu;
	float scale = exp2(float(e) - 15.0 - 9.0);
	return vec3(float(sr), float(sg), float(sb)) * scale;
}

const vec3 aniso_dir[6] = vec3[](
		vec3(1, 0, 0), vec3(0, 1, 0), vec3(0, 0, 1),
		vec3(-1, 0, 0), vec3(0, -1, 0), vec3(0, 0, -1));

void main() {
	ivec3 local = ivec3(gl_GlobalInvocationID);
	if (any(greaterThanEqual(local, pc.slab_dim))) {
		return;
	}
	// Render targets are in direct grid coords; for a streamed shell only [slab_lo, slab_lo+dim)
	// was rasterized. The toroidal voxel cell wraps via the phase (see rc_trace_inc fract()).
	ivec3 gp = pc.slab_lo + local;
	ivec3 cell = (pc.phase + gp) % int(pc.res);

	uint a = imageLoad(src_albedo, gp).r;
	if ((a & 0x1u) == 0u) { // empty voxel
		imageStore(voxel_out, cell, vec4(0.0));
		imageStore(albedo_out, cell, vec4(0.0));
		imageStore(normal_out, cell, vec4(0.5, 0.5, 0.5, 0.0));
		imageStore(emission_out, cell, vec4(0.0));
		return;
	}

	vec3 albedo = vec3(
							  float((a >> 11) & 0x1Fu),
							  float((a >> 6) & 0x1Fu),
							  float((a >> 1) & 0x1Fu)) /
			31.0;

	uint facing = imageLoad(src_facing, gp).r;
	vec3 n = vec3(0.0);
	for (uint i = 0u; i < 6u; i++) {
		if ((facing & (1u << i)) != 0u) {
			n += aniso_dir[i];
		}
	}
	n = (dot(n, n) > 0.0001) ? normalize(n) : vec3(0.0, 1.0, 0.0);

	vec3 emission = decode_rgbe8985(imageLoad(src_emission, gp).r);

	imageStore(voxel_out, cell, vec4(0.0, 0.0, 0.0, 1.0)); // occupancy; radiance from inject
	imageStore(albedo_out, cell, vec4(albedo, 1.0));
	imageStore(normal_out, cell, vec4(n * 0.5 + 0.5, 1.0));
	imageStore(emission_out, cell, vec4(emission, 1.0));
}
