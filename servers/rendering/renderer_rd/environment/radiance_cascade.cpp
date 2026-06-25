/**************************************************************************/
/*  radiance_cascade.cpp                                                  */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2026-present Aleks Stoyanov.                             */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#include "radiance_cascade.h"

// NOTE: This is the in-engine port scaffold of the Radiance Cascades GI solver
// (previously a GDExtension CompositorEffect). The method bodies below are stubs;
// they are filled in pass-by-pass from the reference implementation, each verified
// against a build. See radiance_cascade.h for the architecture overview.

using namespace RendererRD;

/* SHARED SHADERS (owned by GI) */

void RadianceCascadeShaders::init() {
	RD *rd = RD::get_singleton();

	// Each Radiance Cascades pass is a single-variant compute shader: compile the
	// default variant and build its pipeline. patch_trace is the exception -- its
	// variant selects the scene-representation backend the cones sample (set 2).
	auto init_compute = [rd](ShaderRD &p_shader, RID &r_version, RID &r_pipeline, const String &p_define) {
		Vector<String> variants;
		variants.push_back(p_define);
		p_shader.initialize(variants);
		r_version = p_shader.version_create();
		r_pipeline = rd->compute_pipeline_create(p_shader.version_get_shader(r_version, 0));
	};

	// Probe build / trace / merge / gather.
	init_compute(patch_clear, patch_clear_shader, patch_clear_pipeline, "");
	init_compute(patch_rebuild, patch_rebuild_shader, patch_rebuild_pipeline, "");
	init_compute(patch_add, patch_add_shader, patch_add_pipeline, "");
	init_compute(patch_indirect, patch_indirect_shader, patch_indirect_pipeline, "");
	init_compute(patch_trace, patch_trace_shader, patch_trace_pipeline, "\n#define RC_BACKEND_VOXEL\n");
	init_compute(patch_neighbours, patch_neighbours_shader, patch_neighbours_pipeline, "");
	init_compute(patch_merge, patch_merge_shader, patch_merge_pipeline, "");
	init_compute(patch_reduce, patch_reduce_shader, patch_reduce_pipeline, "");
	init_compute(patch_gather, patch_gather_shader, patch_gather_pipeline, "");
	init_compute(patch_lookup, patch_lookup_shader, patch_lookup_pipeline, "");

	// Voxel scene representation (the default trace backend).
	init_compute(voxelize_mesh, voxelize_mesh_shader, voxelize_mesh_pipeline, "");
	init_compute(voxelize_dynamic, voxelize_dynamic_shader, voxelize_dynamic_pipeline, "");
	init_compute(slab_clear, slab_clear_shader, slab_clear_pipeline, "");
	init_compute(voxel_inject, voxel_inject_shader, voxel_inject_pipeline, "");
	init_compute(clip_inject, clip_inject_shader, clip_inject_pipeline, "");
	init_compute(voxel_sdf, voxel_sdf_shader, voxel_sdf_pipeline, "");
	init_compute(voxel_mip_aniso, voxel_mip_aniso_shader, voxel_mip_aniso_pipeline, "");
	init_compute(voxel_emission_mip, voxel_emission_mip_shader, voxel_emission_mip_pipeline, "");
	init_compute(dyn_occ_temporal, dyn_occ_temporal_shader, dyn_occ_temporal_pipeline, "");
	init_compute(voxel_debug, voxel_debug_shader, voxel_debug_pipeline, "");

	// Screen-space irradiance chain.
	init_compute(irradiance_atrous, irradiance_atrous_shader, irradiance_atrous_pipeline, "");
	init_compute(irradiance_upsample, irradiance_upsample_shader, irradiance_upsample_pipeline, "");
	init_compute(composite, composite_shader, composite_pipeline, ""); // TEMPORARY bring-up output
}

void RadianceCascadeShaders::free() {
	RD *rd = RD::get_singleton();

	auto free_compute = [rd](ShaderRD &p_shader, RID &r_version, RID &r_pipeline) {
		if (r_pipeline.is_valid()) {
			rd->free_rid(r_pipeline);
			r_pipeline = RID();
		}
		if (r_version.is_valid()) {
			p_shader.version_free(r_version);
			r_version = RID();
		}
	};

	free_compute(patch_clear, patch_clear_shader, patch_clear_pipeline);
	free_compute(patch_rebuild, patch_rebuild_shader, patch_rebuild_pipeline);
	free_compute(patch_add, patch_add_shader, patch_add_pipeline);
	free_compute(patch_indirect, patch_indirect_shader, patch_indirect_pipeline);
	free_compute(patch_trace, patch_trace_shader, patch_trace_pipeline);
	free_compute(patch_neighbours, patch_neighbours_shader, patch_neighbours_pipeline);
	free_compute(patch_merge, patch_merge_shader, patch_merge_pipeline);
	free_compute(patch_reduce, patch_reduce_shader, patch_reduce_pipeline);
	free_compute(patch_gather, patch_gather_shader, patch_gather_pipeline);
	free_compute(patch_lookup, patch_lookup_shader, patch_lookup_pipeline);

	free_compute(voxelize_mesh, voxelize_mesh_shader, voxelize_mesh_pipeline);
	free_compute(voxelize_dynamic, voxelize_dynamic_shader, voxelize_dynamic_pipeline);
	free_compute(slab_clear, slab_clear_shader, slab_clear_pipeline);
	free_compute(voxel_inject, voxel_inject_shader, voxel_inject_pipeline);
	free_compute(clip_inject, clip_inject_shader, clip_inject_pipeline);
	free_compute(voxel_sdf, voxel_sdf_shader, voxel_sdf_pipeline);
	free_compute(voxel_mip_aniso, voxel_mip_aniso_shader, voxel_mip_aniso_pipeline);
	free_compute(voxel_emission_mip, voxel_emission_mip_shader, voxel_emission_mip_pipeline);
	free_compute(dyn_occ_temporal, dyn_occ_temporal_shader, dyn_occ_temporal_pipeline);
	free_compute(voxel_debug, voxel_debug_shader, voxel_debug_pipeline);

	free_compute(irradiance_atrous, irradiance_atrous_shader, irradiance_atrous_pipeline);
	free_compute(irradiance_upsample, irradiance_upsample_shader, irradiance_upsample_pipeline);
	free_compute(composite, composite_shader, composite_pipeline);
}

/* RADIANCE CASCADE (per-viewport render-buffer custom data) */

RadianceCascade::~RadianceCascade() {
	free_resources();
}

void RadianceCascade::free_data() {
	free_resources();
}

void RadianceCascade::free_resources() {
	// TODO(port): release every per-viewport RID allocated in create().
}

void RadianceCascade::create(GI *p_gi, const Size2i &p_size) {
	gi = p_gi;
	rd = RD::get_singleton();
	screen_size = p_size;
	half_size = Size2i((p_size.x + 1) / 2, (p_size.y + 1) / 2);

	auto make_tex = [this](RD::DataFormat p_format, RD::TextureType p_type, uint32_t p_w, uint32_t p_h, uint32_t p_d, uint32_t p_mips, uint32_t p_usage) -> RID {
		RD::TextureFormat tf;
		tf.format = p_format;
		tf.texture_type = p_type;
		tf.width = p_w;
		tf.height = p_h;
		tf.depth = p_d;
		tf.array_layers = 1;
		tf.mipmaps = p_mips;
		tf.usage_bits = p_usage;
		RD::TextureView tv;
		return rd->texture_create(tf, tv);
	};
	auto make_sampler = [this](RD::SamplerFilter p_filter, RD::SamplerFilter p_mip) -> RID {
		RD::SamplerState ss;
		ss.min_filter = p_filter;
		ss.mag_filter = p_filter;
		ss.mip_filter = p_mip;
		ss.repeat_u = RD::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE;
		ss.repeat_v = RD::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE;
		ss.repeat_w = RD::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE;
		return rd->sampler_create(ss);
	};
	auto filled_buffer = [this](uint32_t p_size, uint8_t p_fill) -> RID {
		Vector<uint8_t> data;
		data.resize(p_size);
		memset(data.ptrw(), p_fill, p_size);
		return rd->storage_buffer_create(p_size, Span<uint8_t>(data.ptr(), data.size()));
	};

	const uint32_t usage_grid = RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_UPDATE_BIT | RD::TEXTURE_USAGE_CAN_COPY_TO_BIT;
	const uint32_t usage_occ = RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_COPY_TO_BIT;
	const uint32_t usage_attr = RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_CAN_UPDATE_BIT;
	const uint32_t grid_mips = 1 + (uint32_t)Math::floor(Math::log((double)vox_res) / Math::log(2.0));
	const uint32_t hres = (uint32_t)vox_res / 2;

	camera_ubo = rd->uniform_buffer_create(sizeof(RCCameraData));
	tri_buffer = rd->storage_buffer_create((uint32_t)(MAX_STATIC_TRIS * FLOATS_PER_TRI * sizeof(float)));
	light_buffer = rd->storage_buffer_create(MAX_LIGHTS * sizeof(RCLightData));

	// Level-0 voxel grid: radiance (rgba16f, full mips) + albedo/normal (rgba8).
	voxel_tex = make_tex(RD::DATA_FORMAT_R16G16B16A16_SFLOAT, RD::TEXTURE_TYPE_3D, vox_res, vox_res, vox_res, grid_mips, usage_grid);
	voxel_albedo = make_tex(RD::DATA_FORMAT_R8G8B8A8_UNORM, RD::TEXTURE_TYPE_3D, vox_res, vox_res, vox_res, 1, usage_attr);
	voxel_normal = make_tex(RD::DATA_FORMAT_R8G8B8A8_UNORM, RD::TEXTURE_TYPE_3D, vox_res, vox_res, vox_res, 1, usage_attr);

	// Dynamic occupancy + its persistent temporal accumulator (r8).
	dyn_occ = make_tex(RD::DATA_FORMAT_R8_UNORM, RD::TEXTURE_TYPE_3D, vox_res, vox_res, vox_res, 1, usage_occ);
	dyn_occ_acc = make_tex(RD::DATA_FORMAT_R8_UNORM, RD::TEXTURE_TYPE_3D, vox_res, vox_res, vox_res, 1, usage_occ);
	rd->texture_clear(dyn_occ_acc, Color(0, 0, 0, 0), 0, 1, 0, 1);

	// Jump-flood SDF + its two ping-pong seeds (half grid resolution).
	sdf_tex = make_tex(RD::DATA_FORMAT_R16_SFLOAT, RD::TEXTURE_TYPE_3D, hres, hres, hres, 1, usage_grid);
	sdf_seed_a = make_tex(RD::DATA_FORMAT_R16G16B16A16_SFLOAT, RD::TEXTURE_TYPE_3D, hres, hres, hres, 1, usage_grid);
	sdf_seed_b = make_tex(RD::DATA_FORMAT_R16G16B16A16_SFLOAT, RD::TEXTURE_TYPE_3D, hres, hres, hres, 1, usage_grid);

	// Six directional radiance mip stacks (cone tracing samples the facing stack).
	vox_mip_levels = (int)grid_mips;
	aniso_levels = CLIP_ANISO_M;
	for (int dir = 0; dir < 6; dir++) {
		voxel_aniso[dir] = make_tex(RD::DATA_FORMAT_R16G16B16A16_SFLOAT, RD::TEXTURE_TYPE_3D, hres, hres, hres, (uint32_t)aniso_levels, RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_SAMPLING_BIT);
		aniso_views[dir].clear();
		for (int L = 0; L < aniso_levels; L++) {
			RD::TextureView tv;
			aniso_views[dir].push_back(rd->texture_create_shared_from_slice(tv, voxel_aniso[dir], 0, L, 1, RD::TEXTURE_SLICE_3D));
		}
	}

	// Emission grid (rgb bounce, full mips) + per-level views for the mip pass.
	voxel_emission = make_tex(RD::DATA_FORMAT_R16G16B16A16_SFLOAT, RD::TEXTURE_TYPE_3D, vox_res, vox_res, vox_res, grid_mips, usage_grid);
	emission_mip_views.clear();
	for (int L = 0; L < vox_mip_levels; L++) {
		RD::TextureView tv;
		emission_mip_views.push_back(rd->texture_create_shared_from_slice(tv, voxel_emission, 0, L, 1, RD::TEXTURE_SLICE_3D));
	}

	// 1^3 black dummy filling unused coarse bindings.
	dummy_clip_tex = make_tex(RD::DATA_FORMAT_R16G16B16A16_SFLOAT, RD::TEXTURE_TYPE_3D, 1, 1, 1, 1, usage_grid | RD::TEXTURE_USAGE_SAMPLING_BIT);
	rd->texture_clear(dummy_clip_tex, Color(0, 0, 0, 0), 0, 1, 0, 1);

	{ // ClipParams UBO -- filled later by the trace-params update.
		struct GpuLevel {
			float origin[3];
			float voxel_size;
			float extent[3];
			float pad;
		};
		struct GpuClip {
			GpuLevel lvl[MAX_CLIP];
			uint32_t num_levels;
			uint32_t pad[3];
		};
		clip_params_ubo = rd->uniform_buffer_create(sizeof(GpuClip));
	}

	// Coarse clipmap radiance grids (levels 1..N) + shared coarse-voxelize scratch.
	for (int L = 1; L < MAX_CLIP; L++) {
		clip_grid[L] = make_tex(RD::DATA_FORMAT_R16G16B16A16_SFLOAT, RD::TEXTURE_TYPE_3D, vox_res, vox_res, vox_res, 1, usage_grid);
	}
	voxel_sampler = make_sampler(RD::SAMPLER_FILTER_LINEAR, RD::SAMPLER_FILTER_LINEAR);
	clip_albedo = make_tex(RD::DATA_FORMAT_R8G8B8A8_UNORM, RD::TEXTURE_TYPE_3D, vox_res, vox_res, vox_res, 1, usage_grid);
	clip_normal = make_tex(RD::DATA_FORMAT_R8G8B8A8_UNORM, RD::TEXTURE_TYPE_3D, vox_res, vox_res, vox_res, 1, usage_grid);
	clip_emission = make_tex(RD::DATA_FORMAT_R16G16B16A16_SFLOAT, RD::TEXTURE_TYPE_3D, vox_res, vox_res, vox_res, 1, usage_grid);

	// Probe store: the cascade table sizes every parallel SSBO below.
	build_cascade_table();
	patch_alloc = rd->storage_buffer_create(sizeof(uint32_t) * MAX_CASCADES);
	patch_keys = rd->storage_buffer_create(total_probes * sizeof(int32_t) * 4); // ivec4 key/slot
	patch_world = rd->storage_buffer_create(total_probes * sizeof(float) * 4); // vec4 world pos
	probe_radiance = rd->storage_buffer_create(total_rad * sizeof(uint32_t)); // packed rgba, 4 B/(probe,dir)
	patch_neighbours = rd->storage_buffer_create(total_probes * 8u * sizeof(uint32_t)); // merge neighbour cache
	probe_inspect_buffer = filled_buffer(512 * sizeof(uint32_t), 0); // debug readback
	probe_rad_tag = filled_buffer(total_probes * sizeof(uint32_t), 0xff); // owner hash, INVALID -> bootstrap
	patch_buckets = rd->storage_buffer_create(total_buckets * sizeof(uint32_t) * 2); // transient hashmap (cleared each frame)
	probe_last_seen = filled_buffer(total_probes * sizeof(uint32_t), 0); // 0 = free id
	patch_live = rd->storage_buffer_create(total_probes * sizeof(uint32_t)); // compact live-id list
	patch_freelist = rd->storage_buffer_create(total_probes * sizeof(uint32_t)); // recycled dense ids
	patch_alloc_state = filled_buffer(MAX_CASCADES * 2 * sizeof(uint32_t), 0); // (free_top, next_id) per cascade
	patch_indirect_buffer = rd->storage_buffer_create(sizeof(uint32_t) * 3 * 2 * MAX_CASCADES, Span<uint8_t>(), RD::STORAGE_BUFFER_USAGE_DISPATCH_INDIRECT);

	// Angular pre-reduce scratch: peak over the r>1 folds (dense id space).
	uint32_t reduced_max = 0;
	for (uint32_t c = 0; c + 1u < MAX_CASCADES; c++) {
		uint32_t r = MAX(cascades[c + 1].oct_res / cascades[c].oct_res, 1u);
		if (r > 1u) {
			reduced_max = MAX(reduced_max, cascades[c + 1].probe_cap * cascades[c].dirs);
		}
	}
	reduced_radiance = rd->storage_buffer_create(reduced_max * sizeof(uint32_t));

	voxel_linear_sampler = make_sampler(RD::SAMPLER_FILTER_LINEAR, RD::SAMPLER_FILTER_LINEAR);
	dyn_tri_buffer = filled_buffer((uint32_t)(sizeof(float) * 12 * MAX_DYN_TRIS), 0);

	// Debug target + the screen-space irradiance chain (gather half-res -> upsample full-res).
	const uint32_t usage_2d = RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT;
	debug_tex = make_tex(RD::DATA_FORMAT_R8G8B8A8_UNORM, RD::TEXTURE_TYPE_2D, screen_size.x, screen_size.y, 1, 1, usage_2d);
	irradiance_tex = make_tex(RD::DATA_FORMAT_R16G16B16A16_SFLOAT, RD::TEXTURE_TYPE_2D, screen_size.x, screen_size.y, 1, 1, usage_2d);
	irradiance_half = make_tex(RD::DATA_FORMAT_R16G16B16A16_SFLOAT, RD::TEXTURE_TYPE_2D, half_size.x, half_size.y, 1, 1, usage_2d);
	irradiance_half_b = make_tex(RD::DATA_FORMAT_R16G16B16A16_SFLOAT, RD::TEXTURE_TYPE_2D, half_size.x, half_size.y, 1, 1, usage_2d);

	point_sampler = make_sampler(RD::SAMPLER_FILTER_NEAREST, RD::SAMPLER_FILTER_NEAREST);
	depth_sampler = make_sampler(RD::SAMPLER_FILTER_LINEAR, RD::SAMPLER_FILTER_LINEAR);
	normal_sampler = make_sampler(RD::SAMPLER_FILTER_NEAREST, RD::SAMPLER_FILTER_NEAREST);
	color_sampler = make_sampler(RD::SAMPLER_FILTER_LINEAR, RD::SAMPLER_FILTER_NEAREST);
	albedo_sampler = make_sampler(RD::SAMPLER_FILTER_LINEAR, RD::SAMPLER_FILTER_NEAREST);

	// Dummy 1x1 HDR placeholders: black scene-color fallback, white albedo (no tint).
	dummy_color_tex = make_tex(RD::DATA_FORMAT_R16G16B16A16_SFLOAT, RD::TEXTURE_TYPE_2D, 1, 1, 1, 1, RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_UPDATE_BIT | RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT);
	{
		Vector<uint8_t> black;
		black.resize(8); // 4 channels * fp16
		memset(black.ptrw(), 0, black.size());
		rd->texture_update(dummy_color_tex, 0, black);
	}
	dummy_albedo_tex = make_tex(RD::DATA_FORMAT_R16G16B16A16_SFLOAT, RD::TEXTURE_TYPE_2D, 1, 1, 1, 1, RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_UPDATE_BIT);
	{
		Vector<uint8_t> white;
		white.resize(8);
		uint8_t *w = white.ptrw();
		const uint16_t one = 0x3c00; // fp16 1.0
		for (int i = 0; i < 4; i++) {
			w[i * 2] = one & 0xff;
			w[i * 2 + 1] = (one >> 8) & 0xff;
		}
		rd->texture_update(dummy_albedo_tex, 0, white);
	}

	// TODO(port): update_trace_params() (set 2 trace-backend UBO) + build the static
	// uniform sets; then process() can run the chain. Next unit.
}

void RadianceCascade::process(RenderDataRD *p_render_data) {
	// TODO(port): the full per-frame chain (see header), writing RB_TEX_AMBIENT.
}

/* CASCADE TABLE */

void RadianceCascade::build_cascade_table() {
	// Define the cascade hierarchy and lay out its GPU buffers. Per cascade c:
	//   spacing       -- probe spacing in world units (grows by cascade_scale^c)
	//   t_start/t_end -- the ray interval it covers; each cascade picks up where the
	//                    finer one stopped, so together they tile [0, far) once.
	//   oct_res/dirs/aperture -- angular resolution (dirs = oct_res^2 octahedral dirs)
	//   *_off/*_cap   -- this cascade's slice of the shared slot space. The hashmap
	//                    (buckets) and the dense probe pool are separate spaces:
	//                    bucket_off/_cap index the transient map (2*pcap, load 0.5);
	//                    probe_off indexes the dense id pool (pcap); rad_off indexes
	//                    the dense radiance (pcap*dirs). Cheap to rebuild from knobs.
	// Uses the member rd, which create() sets before calling this.
	const float ray0 = 0.25;
	const float interval_factor = 4.0;
	const uint32_t oct[MAX_CASCADES] = { 4, 4, 8, 8, 8 };
	const uint32_t pcap[MAX_CASCADES] = { 1u << 20, 1u << 19, 1u << 16, 1u << 14, 1u << 12 };
	const float spacing0 = 0.25;

	uint32_t boff = 0, poff = 0, roff = 0;
	float t = 0.0;
	for (uint32_t c = 0; c < MAX_CASCADES; c++) {
		RCCascadeDesc &cd = cascades[c];
		float kc = Math::pow(cascade_scale, (float)c);

		cd.spacing = spacing0 * kc * dist_mult;
		cd.oct_res = oct[c];
		cd.dirs = oct[c] * oct[c];
		cd.aperture = 2.0 / float(oct[c]);

		float len = ray0 * kc * interval_factor * step_mult;
		cd.t_start = t;
		// Near cone overruns the seam by interval_overlap so it (not the offset coarse
		// probes) owns occlusion there; the next cascade still starts at the seam.
		cd.t_end = t + len * (1.0 + interval_overlap);
		t += len;

		cd.probe_cap = pcap[c];
		cd.bucket_cap = pcap[c] * 2u;
		cd.bucket_off = boff;
		cd.probe_off = poff;
		cd.rad_off = roff;
		cd.pad = 0;

		boff += cd.bucket_cap;
		poff += cd.probe_cap;
		roff += cd.probe_cap * cd.dirs;
	}
	total_buckets = boff;
	total_probes = poff;
	total_rad = roff;

	if (cascade_buffer.is_null()) {
		cascade_buffer = rd->storage_buffer_create(sizeof(cascades), Span<uint8_t>((const uint8_t *)cascades, sizeof(cascades)));
	} else {
		rd->buffer_update(cascade_buffer, 0, sizeof(cascades), cascades);
	}
}

/* INPUTS (engine-native render data, not the SceneTree) */

void RadianceCascade::update_camera(const Projection &p_projection, const Transform3D &p_transform) {
	// TODO(port): pack RCCameraData (inverse + forward view/proj) into camera_ubo.
}

void RadianceCascade::update_lights(RenderDataRD *p_render_data) {
	// TODO(port): gather the renderer's light instances into light_buffer.
}

void RadianceCascade::update_geometry(RenderDataRD *p_render_data) {
	// TODO(port): feed the voxelizer from the render geometry instance list.
}

/* PER-PASS DISPATCH */

void RadianceCascade::dispatch_patch_clear() {}
void RadianceCascade::dispatch_patch_rebuild() {}
void RadianceCascade::dispatch_patch_add() {}
void RadianceCascade::dispatch_patch_trace() {}
void RadianceCascade::dispatch_patch_neighbours() {}
void RadianceCascade::dispatch_patch_merge() {}
void RadianceCascade::dispatch_patch_gather() {}
void RadianceCascade::dispatch_patch_lookup(uint32_t p_debug_kind) {}
void RadianceCascade::dispatch_voxel_mips() {}
void RadianceCascade::dispatch_emission_mips() {}
void RadianceCascade::dispatch_voxel_debug() {}
void RadianceCascade::dispatch_dynamic_voxelize() {}
void RadianceCascade::dispatch_dyn_occ_temporal() {}
void RadianceCascade::dispatch_irradiance_atrous() {}
void RadianceCascade::dispatch_irradiance_upsample() {}
void RadianceCascade::dispatch_composite() {}

/* VOXEL SCENE / SDF */

void RadianceCascade::bake_voxels() {}
void RadianceCascade::build_sdf() {}
