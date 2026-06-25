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

#include "servers/rendering/renderer_rd/environment/gi.h"
#include "servers/rendering/renderer_rd/storage_rd/light_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/render_data_rd.h"
#include "servers/rendering/renderer_rd/storage_rd/render_scene_data_rd.h"

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
	init_compute(voxel_unpack, voxel_unpack_shader, voxel_unpack_pipeline, "");
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
	free_compute(voxel_unpack, voxel_unpack_shader, voxel_unpack_pipeline);
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

	// Toroidal phase: the grid cell the world-origin voxel maps to (the trace samples
	// the grid as fract(W / vox_extent), so a cell holds world_voxel % res).
	{
		const float vsize = vox_extent.x / float(vox_res);
		const int ox = (int)Math::floor(vox_origin.x / vsize);
		const int oy = (int)Math::floor(vox_origin.y / vsize);
		const int oz = (int)Math::floor(vox_origin.z / vsize);
		vox_phase = Vector3i(((ox % vox_res) + vox_res) % vox_res, ((oy % vox_res) + vox_res) % vox_res, ((oz % vox_res) + vox_res) % vox_res);
	}

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

	// Geometry voxelization render targets (packed, SDFGI PASS_MODE_SDF output format).
	const uint32_t usage_pack = RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_COPY_TO_BIT | RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT;
	render_albedo = make_tex(RD::DATA_FORMAT_R16_UINT, RD::TEXTURE_TYPE_3D, vox_res, vox_res, vox_res, 1, usage_pack);
	render_emission = make_tex(RD::DATA_FORMAT_R32_UINT, RD::TEXTURE_TYPE_3D, vox_res, vox_res, vox_res, 1, usage_pack);
	render_emission_aniso = make_tex(RD::DATA_FORMAT_R32_UINT, RD::TEXTURE_TYPE_3D, vox_res, vox_res, vox_res, 1, usage_pack);
	render_geom_facing = make_tex(RD::DATA_FORMAT_R32_UINT, RD::TEXTURE_TYPE_3D, vox_res, vox_res, vox_res, 1, usage_pack);

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
	// Cleared because the trace samples them but only L0 is voxelized for now.
	for (int L = 1; L < MAX_CLIP; L++) {
		clip_grid[L] = make_tex(RD::DATA_FORMAT_R16G16B16A16_SFLOAT, RD::TEXTURE_TYPE_3D, vox_res, vox_res, vox_res, 1, usage_grid);
		rd->texture_clear(clip_grid[L], Color(0, 0, 0, 0), 0, 1, 0, 1);
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

	// Trace-backend UBOs (set 2) then the static uniform sets that wire every pass.
	update_trace_params();
	build_static_sets();
}

void RadianceCascade::process(RenderDataRD *p_render_data, RID p_depth, RID p_normal_roughness, RID p_color) {
	// One frame_index for the whole frame: rebuild/add/trace/merge all read it (eviction
	// age, last_seen stamp, amortization rotation), so advance it once up front.
	frame_index++;

	// Camera from this frame's render data (engine-native -- no SceneTree).
	const Transform3D cam_to_world = p_render_data->scene_data->cam_transform;
	const Projection projection = p_render_data->scene_data->cam_projection;
	z_near = (float)projection.get_z_near();
	z_far = (float)projection.get_z_far();
	update_camera(projection, cam_to_world.affine_inverse());
	update_lights(p_render_data);

	// Per-frame inputs + their set-1 descriptor sets.
	frame_depth = p_depth;
	frame_normal = p_normal_roughness;
	frame_color = p_color;
	rebuild_per_frame_sets();
	if (patch_add_set1.is_null() || patch_lookup_set1.is_null()) {
		return; // no depth this frame
	}

	// If the renderer just (re)voxelized the scene, unpack the packed targets into the
	// voxel grid (albedo/normal/emission + occupancy). Inject/SDF/mips -- which light
	// the grid for the trace -- are the next step; until then the trace sees geometry
	// but no radiance.
	if (voxel_unpack_pending) {
		dispatch_voxel_unpack();
		bake_voxels(); // SDF -> inject -> aniso/emission mips: light the grid for the trace
		voxel_unpack_pending = false;
	}

	// Per-frame probe chain. The voxel grid is still empty (the engine-native geometry
	// feed is the next unit), so this resolves to sky/ambient -- enough to validate the
	// full pipeline (uniform sets, dispatch, shaders) at runtime.
	dispatch_patch_clear();
	dispatch_patch_rebuild();
	dispatch_patch_add();
	dispatch_patch_trace();
	dispatch_patch_neighbours();
	dispatch_patch_merge();
	dispatch_patch_gather();
	dispatch_irradiance_atrous();
	dispatch_irradiance_upsample();
	dispatch_composite();
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

void RadianceCascade::update_trace_params() {
	// Refresh the UBOs the trace backend reads: level-0 placement (trace_params_ubo)
	// and the per-level origin/size table for every clip level (clip_params_ubo).
	RCTraceParams p = {};
	p.vox_origin[0] = vox_origin.x;
	p.vox_origin[1] = vox_origin.y;
	p.vox_origin[2] = vox_origin.z;
	p.vox_extent[0] = vox_extent.x;
	p.vox_extent[1] = vox_extent.y;
	p.vox_extent[2] = vox_extent.z;
	p.voxel_size = vox_extent.x / float(vox_res);
	p.max_steps = (uint32_t)trace_max_steps;
	if (trace_params_ubo.is_null()) {
		trace_params_ubo = rd->uniform_buffer_create(sizeof(p), Span<uint8_t>((const uint8_t *)&p, sizeof(p)));
	} else {
		rd->buffer_update(trace_params_ubo, 0, sizeof(p), &p);
	}

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
	} gc = {};
	const float base = vox_extent.x / float(vox_res);
	for (int L = 0; L < MAX_CLIP; L++) {
		const float vs = base * float(1 << L);
		const float ext = float(vox_res) * vs;
		const Vector3 o = (L == 0) ? vox_origin : clip_origin[L];
		gc.lvl[L] = { { o.x, o.y, o.z }, vs, { ext, ext, ext }, 0.0f };
	}
	gc.num_levels = (uint32_t)clip_levels;
	rd->buffer_update(clip_params_ubo, 0, sizeof(gc), &gc);
}

void RadianceCascade::build_static_sets() {
	// Wire each pass's static resources (set 0, plus trace set 2) into descriptor sets
	// once. Per-frame inputs (depth/normal/color) live in set 1, rebuilt every frame.
	// Convention across the patch passes: cascade table SSBO at b7, camera UBO at b5.
	RadianceCascadeShaders &sh = *gi->rc_shader;
#define RC_SHADER(m) sh.m.version_get_shader(sh.m##_shader, 0)

	auto ssbo = [](int p_bind, RID p_id) {
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
		u.binding = p_bind;
		u.append_id(p_id);
		return u;
	};
	auto ubo = [](int p_bind, RID p_id) {
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_UNIFORM_BUFFER;
		u.binding = p_bind;
		u.append_id(p_id);
		return u;
	};
	auto img = [](int p_bind, RID p_id) {
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_IMAGE;
		u.binding = p_bind;
		u.append_id(p_id);
		return u;
	};
	auto tex = [](int p_bind, RID p_sampler, RID p_tex) {
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE;
		u.binding = p_bind;
		u.append_id(p_sampler);
		u.append_id(p_tex);
		return u;
	};

	{ // composite set0 (TEMPORARY bring-up output): irradiance read + albedo + debug.
		Vector<RD::Uniform> u;
		u.push_back(img(0, irradiance_tex));
		u.push_back(tex(1, albedo_sampler, dummy_albedo_tex));
		u.push_back(img(2, debug_tex));
		composite_set0 = rd->uniform_set_create(u, RC_SHADER(composite), 0);
	}

	{ // patch clear
		Vector<RD::Uniform> u;
		u.push_back(ssbo(0, patch_buckets));
		u.push_back(ssbo(1, patch_alloc));
		patch_clear_set0 = rd->uniform_set_create(u, RC_SHADER(patch_clear), 0);
	}

	{ // patch add (+ camera at b5, cascade table at b7)
		Vector<RD::Uniform> u;
		u.push_back(ssbo(0, patch_buckets));
		u.push_back(ssbo(1, patch_alloc));
		u.push_back(ssbo(2, patch_keys));
		u.push_back(ssbo(3, patch_world));
		u.push_back(ssbo(4, patch_live));
		u.push_back(ubo(5, camera_ubo));
		u.push_back(ssbo(7, cascade_buffer));
		u.push_back(ssbo(8, probe_rad_tag));
		u.push_back(ssbo(10, probe_last_seen));
		u.push_back(ssbo(11, patch_freelist));
		u.push_back(ssbo(12, patch_alloc_state));
		patch_add_set0 = rd->uniform_set_create(u, RC_SHADER(patch_add), 0);
	}

	{ // patch rebuild (dense pool -> repopulate hashmap + evict aged ids)
		Vector<RD::Uniform> u;
		u.push_back(ssbo(0, patch_buckets));
		u.push_back(ssbo(2, patch_keys));
		u.push_back(ssbo(7, cascade_buffer));
		u.push_back(ssbo(8, probe_last_seen));
		u.push_back(ssbo(11, patch_freelist));
		u.push_back(ssbo(12, patch_alloc_state));
		patch_rebuild_set0 = rd->uniform_set_create(u, RC_SHADER(patch_rebuild), 0);
	}

	{ // patch indirect (cascade table at b2 here)
		Vector<RD::Uniform> u;
		u.push_back(ssbo(0, patch_alloc));
		u.push_back(ssbo(1, patch_indirect_buffer));
		u.push_back(ssbo(2, cascade_buffer));
		patch_indirect_set0 = rd->uniform_set_create(u, RC_SHADER(patch_indirect), 0);
	}

	{ // patch trace set0
		Vector<RD::Uniform> u;
		u.push_back(ssbo(0, patch_buckets));
		u.push_back(ssbo(1, patch_alloc));
		u.push_back(ssbo(3, patch_world));
		u.push_back(ssbo(4, patch_live));
		u.push_back(ssbo(6, probe_radiance));
		u.push_back(ssbo(7, cascade_buffer));
		patch_trace_set0 = rd->uniform_set_create(u, RC_SHADER(patch_trace), 0);
	}

	{ // patch merge (neighbour ids precomputed at b9)
		Vector<RD::Uniform> u;
		u.push_back(ssbo(1, patch_alloc));
		u.push_back(ssbo(3, patch_world));
		u.push_back(ssbo(4, patch_live));
		u.push_back(ssbo(9, patch_neighbours));
		u.push_back(ssbo(6, probe_radiance));
		u.push_back(ssbo(7, cascade_buffer));
		u.push_back(ssbo(8, reduced_radiance));
		patch_merge_set0 = rd->uniform_set_create(u, RC_SHADER(patch_merge), 0);
	}

	{ // patch neighbours precompute (writes neighbours at b9)
		Vector<RD::Uniform> u;
		u.push_back(ssbo(0, patch_buckets));
		u.push_back(ssbo(1, patch_alloc));
		u.push_back(ssbo(2, patch_keys));
		u.push_back(ssbo(3, patch_world));
		u.push_back(ssbo(4, patch_live));
		u.push_back(ssbo(7, cascade_buffer));
		u.push_back(ssbo(9, patch_neighbours));
		patch_neighbours_set0 = rd->uniform_set_create(u, RC_SHADER(patch_neighbours), 0);
	}

	{ // patch reduce (angular pre-reduce; iterates dense ids)
		Vector<RD::Uniform> u;
		u.push_back(ssbo(6, probe_radiance));
		u.push_back(ssbo(7, cascade_buffer));
		u.push_back(ssbo(8, reduced_radiance));
		u.push_back(ssbo(10, probe_last_seen));
		patch_reduce_set0 = rd->uniform_set_create(u, RC_SHADER(patch_reduce), 0);
	}

	{ // trace set2 -- the scene representation a probe ray samples (voxel backend).
		auto clip_tex = [&](int p_bind, int L) {
			RID t = (L < clip_levels && clip_grid[L].is_valid()) ? clip_grid[L] : dummy_clip_tex;
			return tex(p_bind, voxel_sampler, t);
		};
		static const int aniso_bind[6] = { 3, 4, 5, 6, 7, 8 };
		Vector<RD::Uniform> u;
		u.push_back(tex(0, voxel_linear_sampler, voxel_tex));
		u.push_back(ubo(1, trace_params_ubo));
		u.push_back(tex(2, voxel_linear_sampler, dyn_occ_acc));
		for (int dir = 0; dir < 6; dir++) {
			u.push_back(tex(aniso_bind[dir], voxel_linear_sampler, voxel_aniso[dir]));
		}
		u.push_back(tex(9, voxel_linear_sampler, voxel_emission));
		u.push_back(clip_tex(10, 1));
		u.push_back(clip_tex(11, 2));
		u.push_back(clip_tex(12, 3));
		u.push_back(clip_tex(13, 4));
		u.push_back(ubo(14, clip_params_ubo));
		u.push_back(tex(15, voxel_linear_sampler, sdf_tex));
		trace_voxel_set2 = rd->uniform_set_create(u, RC_SHADER(patch_trace), 2);
	}

	{ // patch lookup (debug)
		Vector<RD::Uniform> u;
		u.push_back(ssbo(0, patch_buckets));
		u.push_back(ssbo(2, patch_keys));
		u.push_back(img(4, debug_tex));
		u.push_back(ubo(5, camera_ubo));
		u.push_back(ssbo(6, probe_radiance));
		u.push_back(ssbo(7, cascade_buffer));
		patch_lookup_set0 = rd->uniform_set_create(u, RC_SHADER(patch_lookup), 0);
	}

	{ // voxel debug (level 0)
		Vector<RD::Uniform> u;
		u.push_back(tex(0, voxel_linear_sampler, voxel_tex));
		u.push_back(img(1, debug_tex));
		u.push_back(ubo(2, camera_ubo));
		u.push_back(tex(3, voxel_linear_sampler, voxel_emission));
		voxel_debug_set0 = rd->uniform_set_create(u, RC_SHADER(voxel_debug), 0);
	}

	for (int L = 1; L < MAX_CLIP; L++) { // voxel debug per coarse grid
		if (!clip_grid[L].is_valid()) {
			continue;
		}
		Vector<RD::Uniform> u;
		u.push_back(tex(0, voxel_linear_sampler, clip_grid[L]));
		u.push_back(img(1, debug_tex));
		u.push_back(ubo(2, camera_ubo));
		u.push_back(tex(3, voxel_linear_sampler, clip_grid[L]));
		voxel_debug_clip_set[L] = rd->uniform_set_create(u, RC_SHADER(voxel_debug), 0);
	}

	{ // upsample set0 (static): half irradiance in, full-res out
		Vector<RD::Uniform> u;
		u.push_back(tex(0, point_sampler, irradiance_half));
		u.push_back(img(1, irradiance_tex));
		upsample_set0 = rd->uniform_set_create(u, RC_SHADER(irradiance_upsample), 0);
	}

	{ // a-trous ping-pong set0 (static): half<->scratch
		auto mkset = [&](RID p_in, RID p_out) {
			Vector<RD::Uniform> u;
			u.push_back(tex(0, point_sampler, p_in));
			u.push_back(img(1, p_out));
			return rd->uniform_set_create(u, RC_SHADER(irradiance_atrous), 0);
		};
		atrous_set0_h2s = mkset(irradiance_half, irradiance_half_b);
		atrous_set0_s2h = mkset(irradiance_half_b, irradiance_half);
	}

	{ // upsample set2 -- c0 probes for the edge full-res gather
		Vector<RD::Uniform> u;
		u.push_back(ssbo(0, patch_buckets));
		u.push_back(ssbo(2, patch_keys));
		u.push_back(ubo(5, camera_ubo));
		u.push_back(ssbo(6, probe_radiance));
		u.push_back(ssbo(7, cascade_buffer));
		upsample_set2 = rd->uniform_set_create(u, RC_SHADER(irradiance_upsample), 2);
	}

	{ // gather set0
		Vector<RD::Uniform> u;
		u.push_back(ssbo(0, patch_buckets));
		u.push_back(ssbo(2, patch_keys));
		u.push_back(img(4, irradiance_half));
		u.push_back(ubo(5, camera_ubo));
		u.push_back(ssbo(6, probe_radiance));
		u.push_back(ssbo(7, cascade_buffer));
		u.push_back(ssbo(9, probe_inspect_buffer));
		patch_gather_set0 = rd->uniform_set_create(u, RC_SHADER(patch_gather), 0);
	}

	{ // inject set0 (direct light -> level-0 voxels)
		Vector<RD::Uniform> u;
		u.push_back(img(0, voxel_tex));
		u.push_back(img(1, voxel_albedo));
		u.push_back(img(2, voxel_normal));
		u.push_back(tex(3, voxel_sampler, sdf_tex));
		u.push_back(img(4, voxel_emission));
		u.push_back(ssbo(5, light_buffer));
		inject_set0 = rd->uniform_set_create(u, RC_SHADER(voxel_inject), 0);
	}

	{ // voxel unpack: packed voxelization targets -> RC voxel grid
		Vector<RD::Uniform> u;
		u.push_back(img(0, render_albedo));
		u.push_back(img(1, render_emission));
		u.push_back(img(2, render_geom_facing));
		u.push_back(img(3, voxel_tex));
		u.push_back(img(4, voxel_albedo));
		u.push_back(img(5, voxel_normal));
		u.push_back(img(6, voxel_emission));
		voxel_unpack_set0 = rd->uniform_set_create(u, RC_SHADER(voxel_unpack), 0);
	}

	for (int L = 1; L < MAX_CLIP; L++) { // coarse clipmap voxelize / inject / slab-clear sets
		{
			Vector<RD::Uniform> u;
			u.push_back(img(0, clip_grid[L]));
			u.push_back(ssbo(1, tri_buffer));
			u.push_back(img(2, clip_albedo));
			u.push_back(img(3, clip_normal));
			u.push_back(img(4, clip_emission));
			clip_voxelize_set[L] = rd->uniform_set_create(u, RC_SHADER(voxelize_mesh), 0);
		}
		{
			Vector<RD::Uniform> u;
			u.push_back(img(0, clip_grid[L]));
			u.push_back(img(1, clip_albedo));
			u.push_back(img(2, clip_normal));
			u.push_back(img(3, clip_emission));
			u.push_back(ssbo(4, light_buffer));
			clip_inject_set[L] = rd->uniform_set_create(u, RC_SHADER(clip_inject), 0);
		}
		{
			Vector<RD::Uniform> u;
			u.push_back(img(0, clip_grid[L]));
			u.push_back(img(1, clip_albedo));
			u.push_back(img(2, clip_normal));
			u.push_back(img(3, clip_emission));
			clip_slab_clear_set[L] = rd->uniform_set_create(u, RC_SHADER(slab_clear), 0);
		}
	}

	{ // level-0 voxelize set0
		Vector<RD::Uniform> u;
		u.push_back(img(0, voxel_tex));
		u.push_back(ssbo(1, tri_buffer));
		u.push_back(img(2, voxel_albedo));
		u.push_back(img(3, voxel_normal));
		u.push_back(img(4, voxel_emission));
		voxelize_set0 = rd->uniform_set_create(u, RC_SHADER(voxelize_mesh), 0);
	}

	{ // level-0 slab clear set (4 grids, no tris)
		Vector<RD::Uniform> u;
		u.push_back(img(0, voxel_tex));
		u.push_back(img(1, voxel_albedo));
		u.push_back(img(2, voxel_normal));
		u.push_back(img(3, voxel_emission));
		slab_clear_set = rd->uniform_set_create(u, RC_SHADER(slab_clear), 0);
	}

	{ // dynamic voxelize set0
		Vector<RD::Uniform> u;
		u.push_back(img(0, dyn_occ));
		u.push_back(ssbo(1, dyn_tri_buffer));
		dyn_voxelize_set0 = rd->uniform_set_create(u, RC_SHADER(voxelize_dynamic), 0);
	}

	{ // dynamic occupancy temporal-accumulate set0
		Vector<RD::Uniform> u;
		u.push_back(img(0, dyn_occ));
		u.push_back(img(1, dyn_occ_acc));
		dyn_occ_temporal_set0 = rd->uniform_set_create(u, RC_SHADER(dyn_occ_temporal), 0);
	}

	{ // SDF jump-flood -- write A (seed_out = a)
		Vector<RD::Uniform> u;
		u.push_back(img(0, sdf_seed_b)); // seed_in
		u.push_back(img(1, sdf_seed_a)); // seed_out
		u.push_back(img(2, voxel_tex)); // occupancy (.a)
		u.push_back(img(3, sdf_tex)); // sdf_out
		sdf_set_write_a = rd->uniform_set_create(u, RC_SHADER(voxel_sdf), 0);
	}

	{ // SDF jump-flood -- write B (seed_out = b)
		Vector<RD::Uniform> u;
		u.push_back(img(0, sdf_seed_a));
		u.push_back(img(1, sdf_seed_b));
		u.push_back(img(2, voxel_tex));
		u.push_back(img(3, sdf_tex));
		sdf_set_write_b = rd->uniform_set_create(u, RC_SHADER(voxel_sdf), 0);
	}

#undef RC_SHADER
}

/* INPUTS (engine-native render data, not the SceneTree) */

void RadianceCascade::update_camera(const Projection &p_projection, const Transform3D &p_view) {
	// Pack both directions of the transform: screen->world (inverse) to place probes,
	// world->screen (forward) to reproject. Column-major to match the GLSL mat4 layout.
	if (camera_ubo.is_null()) {
		return;
	}
	RCCameraData cam = {};

	auto copy_proj = [](float *p_dst, const Projection &p) {
		for (int col = 0; col < 4; col++) {
			for (int row = 0; row < 4; row++) {
				p_dst[col * 4 + row] = p.columns[col][row];
			}
		}
	};
	auto copy_transform = [](float *p_dst, const Transform3D &t) {
		for (int col = 0; col < 3; col++) {
			for (int row = 0; row < 3; row++) {
				p_dst[col * 4 + row] = t.basis.rows[row][col];
			}
		}
		p_dst[0 * 4 + 3] = 0.0f;
		p_dst[1 * 4 + 3] = 0.0f;
		p_dst[2 * 4 + 3] = 0.0f;
		p_dst[3 * 4 + 0] = t.origin.x;
		p_dst[3 * 4 + 1] = t.origin.y;
		p_dst[3 * 4 + 2] = t.origin.z;
		p_dst[3 * 4 + 3] = 1.0f;
	};

	copy_proj(cam.inv_proj, p_projection.inverse());
	copy_transform(cam.inv_view, p_view.inverse());
	copy_proj(cam.fwd_proj, p_projection);
	copy_transform(cam.fwd_view, p_view);

	rd->buffer_update(camera_ubo, 0, sizeof(cam), &cam);
}

void RadianceCascade::rebuild_per_frame_sets() {
	// set=1 for the passes that read this frame's buffers (depth / normal-roughness /
	// scene color). Rebuilt every frame because those RIDs can change; free the prior.
	RadianceCascadeShaders &sh = *gi->rc_shader;
	if (frame_depth.is_null()) {
		return;
	}

	auto tex = [](int p_bind, RID p_sampler, RID p_tex) {
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE;
		u.binding = p_bind;
		u.append_id(p_sampler);
		u.append_id(p_tex);
		return u;
	};

	{ // lookup set1: depth(0) + normal(1) -- also bound by gather
		Vector<RD::Uniform> u;
		u.push_back(tex(0, depth_sampler, frame_depth));
		u.push_back(tex(1, normal_sampler, frame_normal));
		if (patch_lookup_set1.is_valid()) {
			rd->free_rid(patch_lookup_set1);
		}
		patch_lookup_set1 = rd->uniform_set_create(u, sh.patch_lookup.version_get_shader(sh.patch_lookup_shader, 0), 1);
	}

	{ // add set1: depth(0) only
		Vector<RD::Uniform> u;
		u.push_back(tex(0, depth_sampler, frame_depth));
		if (patch_add_set1.is_valid()) {
			rd->free_rid(patch_add_set1);
		}
		patch_add_set1 = rd->uniform_set_create(u, sh.patch_add.version_get_shader(sh.patch_add_shader, 0), 1);
	}

	{ // composite set1: scene color bound twice (sampler + image)
		const RID color = frame_color.is_valid() ? frame_color : dummy_color_tex;
		Vector<RD::Uniform> u;
		u.push_back(tex(0, color_sampler, color));
		RD::Uniform u1;
		u1.uniform_type = RD::UNIFORM_TYPE_IMAGE;
		u1.binding = 1;
		u1.append_id(color);
		u.push_back(u1);
		if (composite_set1.is_valid()) {
			rd->free_rid(composite_set1);
		}
		composite_set1 = rd->uniform_set_create(u, sh.composite.version_get_shader(sh.composite_shader, 0), 1);
	}
}

void RadianceCascade::update_lights(RenderDataRD *p_render_data) {
	// Engine-native light feed: read this frame's light instances from LightStorage
	// (directional + omni + spot, all in render_data->lights) into light_buffer. The
	// inject pass lights the whole world-space voxel grid from these, so we use the
	// full light list -- NOT the screen-space Forward+ cluster, which only covers the
	// visible frustum (this is why SDFGI keeps its own light list too).
	RendererRD::LightStorage *light_storage = RendererRD::LightStorage::get_singleton();
	const PagedArray<RID> &light_list = *p_render_data->lights;

	LocalVector<RCLightData> out;
	for (uint32_t i = 0; i < (uint32_t)light_list.size(); i++) {
		if (out.size() >= MAX_LIGHTS) {
			break;
		}
		RID li = light_list[i];
		if (!light_storage->owns_light_instance(li)) {
			continue;
		}
		RID base = light_storage->light_instance_get_base_light(li);
		const Transform3D xf = light_storage->light_instance_get_base_transform(li);
		const RSE::LightType type = light_storage->light_get_type(base);

		RCLightData g = {};
		const Vector3 dir = -xf.basis.get_column(Vector3::AXIS_Z).normalized();
		g.direction[0] = dir.x;
		g.direction[1] = dir.y;
		g.direction[2] = dir.z;
		g.position[0] = xf.origin.x;
		g.position[1] = xf.origin.y;
		g.position[2] = xf.origin.z;

		const Color col = light_storage->light_get_color(base).srgb_to_linear();
		const float energy = light_storage->light_get_param(base, RSE::LIGHT_PARAM_ENERGY) * light_storage->light_get_param(base, RSE::LIGHT_PARAM_INDIRECT_ENERGY);
		g.color[0] = col.r * energy;
		g.color[1] = col.g * energy;
		g.color[2] = col.b * energy;

		if (type == RSE::LIGHT_DIRECTIONAL) {
			g.type = 0.0f;
			g.inv_range = 0.0f;
		} else {
			const float range = light_storage->light_get_param(base, RSE::LIGHT_PARAM_RANGE);
			g.inv_range = range > 0.0f ? 1.0f / range : 0.0f;
			if (type == RSE::LIGHT_OMNI) {
				g.type = 1.0f;
			} else { // spot
				g.type = 2.0f;
				const float spot_angle = light_storage->light_get_param(base, RSE::LIGHT_PARAM_SPOT_ANGLE);
				g.spot_cos_out = Math::cos(Math::deg_to_rad(spot_angle));
				g.spot_cos_in = g.spot_cos_out;
			}
		}
		out.push_back(g);
	}

	light_count = out.size();
	if (light_count > 0) {
		rd->buffer_update(light_buffer, 0, light_count * sizeof(RCLightData), out.ptr());
	}
}

void RadianceCascade::update_geometry(RenderDataRD *p_render_data) {
	// TODO(port): feed the voxelizer from the render geometry instance list.
}

/* PER-PASS DISPATCH */

// Amortization is only correct on a temporally static grid: while a relight cross-fade
// is armed (or a dynamic light is tracking) the grid changes every frame, so force N=1.
uint32_t RadianceCascade::effective_amortization() const {
	const bool relighting = relight_frames > 0 || relight_track_frames > 0 || clip_relight_frames > 0;
	return relighting ? 1u : trace_amortization;
}

void RadianceCascade::dispatch_patch_clear() {
	RadianceCascadeShaders &sh = *gi->rc_shader;
	RCPatchClearPushConstant pc = {};
	pc.total_buckets = total_buckets;
	pc.num_cascades = MAX_CASCADES;
	RD::ComputeListID l = rd->compute_list_begin();
	rd->compute_list_bind_compute_pipeline(l, sh.patch_clear_pipeline);
	rd->compute_list_bind_uniform_set(l, patch_clear_set0, 0);
	rd->compute_list_set_push_constant(l, &pc, sizeof(pc));
	rd->compute_list_dispatch(l, (total_buckets + 255u) / 256u, 1, 1);
	rd->compute_list_end();
}

void RadianceCascade::dispatch_patch_rebuild() {
	// Repopulate the cleared hashmap from the persistent dense pool (one thread per
	// dense id per cascade); aged-out ids are freed to the free-list.
	RadianceCascadeShaders &sh = *gi->rc_shader;
	RD::ComputeListID l = rd->compute_list_begin();
	rd->compute_list_bind_compute_pipeline(l, sh.patch_rebuild_pipeline);
	rd->compute_list_bind_uniform_set(l, patch_rebuild_set0, 0);
	for (uint32_t c = 0; c < MAX_CASCADES; c++) {
		RCPatchRebuildPushConstant pc = {};
		pc.frame = frame_index;
		pc.evict_age = evict_age;
		pc.cascade = c;
		rd->compute_list_set_push_constant(l, &pc, sizeof(pc));
		rd->compute_list_dispatch(l, (cascades[c].probe_cap + 63u) / 64u, 1, 1);
	}
	rd->compute_list_end();
}

void RadianceCascade::dispatch_patch_add() {
	// Persistent find-or-insert. Pass A seeds cascade 0 on the gather's half-res
	// lattice; Pass B seeds the coarse cascades on a lower-density seed lattice.
	RadianceCascadeShaders &sh = *gi->rc_shader;
	RD::ComputeListID l = rd->compute_list_begin();
	rd->compute_list_bind_compute_pipeline(l, sh.patch_add_pipeline);
	rd->compute_list_bind_uniform_set(l, patch_add_set0, 0);
	rd->compute_list_bind_uniform_set(l, patch_add_set1, 1);

	{ // Pass A -- cascade 0, half-res lattice
		RCPatchAddPushConstant pc = {};
		pc.screen_width = (uint32_t)half_size.x;
		pc.screen_height = (uint32_t)half_size.y;
		pc.cascade_begin = 0;
		pc.cascade_end = 1;
		pc.z_near = z_near;
		pc.z_far = z_far;
		pc.frame = frame_index;
		rd->compute_list_set_push_constant(l, &pc, sizeof(pc));
		rd->compute_list_dispatch(l, ((uint32_t)half_size.x + 7u) / 8u, ((uint32_t)half_size.y + 7u) / 8u, 1);
	}
	{ // Pass B -- coarse cascades 1..N-1, seed lattice
		const uint32_t seed_h = MIN((uint32_t)screen_size.y, (uint32_t)probe_seed_max_h);
		const uint32_t seed_w = (uint32_t)Math::round(double(screen_size.x) * double(seed_h) / double(screen_size.y));
		RCPatchAddPushConstant pc = {};
		pc.screen_width = seed_w;
		pc.screen_height = seed_h;
		pc.cascade_begin = 1;
		pc.cascade_end = MAX_CASCADES;
		pc.z_near = z_near;
		pc.z_far = z_far;
		pc.frame = frame_index;
		rd->compute_list_set_push_constant(l, &pc, sizeof(pc));
		rd->compute_list_dispatch(l, (seed_w + 7u) / 8u, (seed_h + 7u) / 8u, 1);
	}
	rd->compute_list_end();
}

void RadianceCascade::dispatch_patch_trace() {
	RadianceCascadeShaders &sh = *gi->rc_shader;
	{ // build N indirect arg-sets from the live per-cascade counts
		RCPatchIndirectPushConstant pc = {};
		pc.num_cascades = MAX_CASCADES;
		pc.local_size = 64;
		RD::ComputeListID l = rd->compute_list_begin();
		rd->compute_list_bind_compute_pipeline(l, sh.patch_indirect_pipeline);
		rd->compute_list_bind_uniform_set(l, patch_indirect_set0, 0);
		rd->compute_list_set_push_constant(l, &pc, sizeof(pc));
		rd->compute_list_dispatch(l, 1, 1, 1);
		rd->compute_list_end();
	}
	{ // trace each cascade over exactly its live probes (indirect)
		const uint32_t eff_amortize = effective_amortization();
		RD::ComputeListID l = rd->compute_list_begin();
		rd->compute_list_bind_compute_pipeline(l, sh.patch_trace_pipeline);
		rd->compute_list_bind_uniform_set(l, patch_trace_set0, 0);
		rd->compute_list_bind_uniform_set(l, trace_voxel_set2, 2);
		for (uint32_t c = 0; c < MAX_CASCADES; c++) {
			RCPatchTracePushConstant pc = {};
			pc.cascade = c;
			pc.local_transmittance = local_transmittance ? 1u : 0u;
			pc.frame = frame_index;
			pc.amortize_n = eff_amortize;
			rd->compute_list_set_push_constant(l, &pc, sizeof(pc));
			rd->compute_list_dispatch_indirect(l, patch_indirect_buffer, c * 12);
		}
		rd->compute_list_end();
	}
}

void RadianceCascade::dispatch_patch_neighbours() {
	// Precompute the 8 trilinear c+1 neighbour ids per live probe so merge reads them
	// instead of running find_in_region inline (frees merge registers -> occupancy).
	RadianceCascadeShaders &sh = *gi->rc_shader;
	RD::ComputeListID l = rd->compute_list_begin();
	rd->compute_list_bind_compute_pipeline(l, sh.patch_neighbours_pipeline);
	rd->compute_list_bind_uniform_set(l, patch_neighbours_set0, 0);
	for (uint32_t c = 0; c + 1u < MAX_CASCADES; c++) {
		RCPatchMergePushConstant pc = {};
		pc.cascade = c;
		rd->compute_list_set_push_constant(l, &pc, sizeof(pc));
		rd->compute_list_dispatch_indirect(l, patch_indirect_buffer, (MAX_CASCADES + c) * 12);
	}
	rd->compute_list_end();
}

void RadianceCascade::dispatch_patch_merge() {
	// Walk far->near, folding cascade c+1 into c. When c+1 has more directions, a
	// pre-reduce pass averages the extra directions into scratch first.
	RadianceCascadeShaders &sh = *gi->rc_shader;
	const uint32_t eff_amortize = effective_amortization();
	for (int c = (int)MAX_CASCADES - 2; c >= 0; c--) {
		const uint32_t r = MAX(cascades[c + 1].oct_res / cascades[c].oct_res, 1u);
		if (r > 1u) {
			RCPatchMergePushConstant rp = {};
			rp.cascade = (uint32_t)c;
			const uint32_t threads = cascades[c + 1].probe_cap * cascades[c].dirs;
			RD::ComputeListID lr = rd->compute_list_begin();
			rd->compute_list_bind_compute_pipeline(lr, sh.patch_reduce_pipeline);
			rd->compute_list_bind_uniform_set(lr, patch_reduce_set0, 0);
			rd->compute_list_set_push_constant(lr, &rp, sizeof(rp));
			rd->compute_list_dispatch(lr, (threads + 63u) / 64u, 1, 1);
			rd->compute_list_end();
		}
		RCPatchMergePushConstant pc = {};
		pc.cascade = (uint32_t)c;
		pc.frame = frame_index;
		pc.amortize_n = eff_amortize;
		RD::ComputeListID l = rd->compute_list_begin();
		rd->compute_list_bind_compute_pipeline(l, sh.patch_merge_pipeline);
		rd->compute_list_bind_uniform_set(l, patch_merge_set0, 0);
		rd->compute_list_set_push_constant(l, &pc, sizeof(pc));
		rd->compute_list_dispatch_indirect(l, patch_indirect_buffer, (MAX_CASCADES + (uint32_t)c) * 12);
		rd->compute_list_end();
	}
}

void RadianceCascade::dispatch_patch_gather() {
	// Per half-res pixel: reconstruct world from depth, sample the covering cascade-0
	// probe into the half-res irradiance buffer (sky fallback where rays escaped).
	RadianceCascadeShaders &sh = *gi->rc_shader;
	RCPatchGatherPushConstant pc = {};
	pc.screen_width = (uint32_t)half_size.x;
	pc.screen_height = (uint32_t)half_size.y;
	pc.z_near = z_near;
	pc.z_far = z_far;
	pc.sky_color[0] = sky_color.x;
	pc.sky_color[1] = sky_color.y;
	pc.sky_color[2] = sky_color.z;
	pc.pad0[0] = 0xffffffffu; // debug inspector disabled
	pc.pad0[1] = 0xffffffffu;
	RD::ComputeListID l = rd->compute_list_begin();
	rd->compute_list_bind_compute_pipeline(l, sh.patch_gather_pipeline);
	rd->compute_list_bind_uniform_set(l, patch_gather_set0, 0);
	rd->compute_list_bind_uniform_set(l, patch_lookup_set1, 1); // per-frame depth/normal
	rd->compute_list_set_push_constant(l, &pc, sizeof(pc));
	rd->compute_list_dispatch(l, ((uint32_t)half_size.x + 7u) / 8u, ((uint32_t)half_size.y + 7u) / 8u, 1);
	rd->compute_list_end();
}

void RadianceCascade::dispatch_irradiance_atrous() {
	// Edge-aware a-trous denoise of the half-res irradiance: ping-pong passes with a
	// doubling hole size, weighted by depth + normal. Even pass count ends in half.
	if (atrous_passes <= 0) {
		return;
	}
	RadianceCascadeShaders &sh = *gi->rc_shader;
	{ // per-frame set1: depth + normal (NEAREST)
		Vector<RD::Uniform> u;
		RD::Uniform ud;
		ud.uniform_type = RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE;
		ud.binding = 0;
		ud.append_id(point_sampler);
		ud.append_id(frame_depth);
		u.push_back(ud);
		RD::Uniform un;
		un.uniform_type = RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE;
		un.binding = 1;
		un.append_id(point_sampler);
		un.append_id(frame_normal);
		u.push_back(un);
		if (atrous_set1.is_valid()) {
			rd->free_rid(atrous_set1);
		}
		atrous_set1 = rd->uniform_set_create(u, sh.irradiance_atrous.version_get_shader(sh.irradiance_atrous_shader, 0), 1);
	}

	RCAtrousPushConstant pc = {};
	pc.half_w = (uint32_t)half_size.x;
	pc.half_h = (uint32_t)half_size.y;
	pc.z_near = z_near;
	pc.z_far = z_far;
	pc.sigma_z = sigma_z;
	pc.normal_pow = normal_pow;
	const uint32_t gx = ((uint32_t)half_size.x + 7u) / 8u;
	const uint32_t gy = ((uint32_t)half_size.y + 7u) / 8u;
	for (int i = 0; i < atrous_passes; i++) {
		pc.step = 1 << i;
		RID s0 = (i & 1) ? atrous_set0_s2h : atrous_set0_h2s;
		RD::ComputeListID l = rd->compute_list_begin();
		rd->compute_list_bind_compute_pipeline(l, sh.irradiance_atrous_pipeline);
		rd->compute_list_bind_uniform_set(l, s0, 0);
		rd->compute_list_bind_uniform_set(l, atrous_set1, 1);
		rd->compute_list_set_push_constant(l, &pc, sizeof(pc));
		rd->compute_list_dispatch(l, gx, gy, 1);
		rd->compute_list_end();
	}
}

void RadianceCascade::dispatch_irradiance_upsample() {
	// Bilateral upsample of the half-res irradiance to full-res, depth/normal weighted;
	// strong edges additionally do a direct full-res cascade-0 gather (set 2).
	RadianceCascadeShaders &sh = *gi->rc_shader;
	{ // per-frame set1: depth + normal
		Vector<RD::Uniform> u;
		RD::Uniform ud;
		ud.uniform_type = RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE;
		ud.binding = 0;
		ud.append_id(point_sampler);
		ud.append_id(frame_depth);
		u.push_back(ud);
		RD::Uniform un;
		un.uniform_type = RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE;
		un.binding = 1;
		un.append_id(point_sampler);
		un.append_id(frame_normal);
		u.push_back(un);
		if (upsample_set1.is_valid()) {
			rd->free_rid(upsample_set1);
		}
		upsample_set1 = rd->uniform_set_create(u, sh.irradiance_upsample.version_get_shader(sh.irradiance_upsample_shader, 0), 1);
	}

	RCUpsamplePushConstant pc = {};
	pc.full_w = (uint32_t)screen_size.x;
	pc.full_h = (uint32_t)screen_size.y;
	pc.half_w = (uint32_t)half_size.x;
	pc.half_h = (uint32_t)half_size.y;
	pc.z_near = z_near;
	pc.z_far = z_far;
	pc.sigma_z = sigma_z;
	pc.normal_pow = normal_pow;
	pc.sky_color[0] = sky_color.x;
	pc.sky_color[1] = sky_color.y;
	pc.sky_color[2] = sky_color.z;
	RD::ComputeListID l = rd->compute_list_begin();
	rd->compute_list_bind_compute_pipeline(l, sh.irradiance_upsample_pipeline);
	rd->compute_list_bind_uniform_set(l, upsample_set0, 0);
	rd->compute_list_bind_uniform_set(l, upsample_set1, 1);
	rd->compute_list_bind_uniform_set(l, upsample_set2, 2);
	rd->compute_list_set_push_constant(l, &pc, sizeof(pc));
	rd->compute_list_dispatch(l, ((uint32_t)screen_size.x + 7u) / 8u, ((uint32_t)screen_size.y + 7u) / 8u, 1);
	rd->compute_list_end();
}

void RadianceCascade::dispatch_composite() {
	// TEMPORARY bring-up output: blend the irradiance over the bound color buffer.
	// Replaced by writing RB_TEX_AMBIENT (then rc_composite is deleted).
	RadianceCascadeShaders &sh = *gi->rc_shader;
	RCCompositePushConstant pc = {};
	pc.screen_width = (uint32_t)screen_size.x;
	pc.screen_height = (uint32_t)screen_size.y;
	pc.gi_intensity = gi_intensity;
	pc.debug_mode = 0;
	RD::ComputeListID l = rd->compute_list_begin();
	rd->compute_list_bind_compute_pipeline(l, sh.composite_pipeline);
	rd->compute_list_bind_uniform_set(l, composite_set0, 0);
	rd->compute_list_bind_uniform_set(l, composite_set1, 1);
	rd->compute_list_set_push_constant(l, &pc, sizeof(pc));
	rd->compute_list_dispatch(l, ((uint32_t)screen_size.x + 7u) / 8u, ((uint32_t)screen_size.y + 7u) / 8u, 1);
	rd->compute_list_end();
}

void RadianceCascade::dispatch_patch_lookup(uint32_t p_debug_kind) {}
void RadianceCascade::dispatch_voxel_unpack() {
	// Unpack the packed voxelization targets into the RC voxel grid (one thread/cell).
	RadianceCascadeShaders &sh = *gi->rc_shader;
	RCVoxelUnpackPushConstant pc = {};
	pc.phase[0] = vox_phase.x;
	pc.phase[1] = vox_phase.y;
	pc.phase[2] = vox_phase.z;
	pc.res = (uint32_t)vox_res;
	const uint32_t g = ((uint32_t)vox_res + 3u) / 4u;
	RD::ComputeListID l = rd->compute_list_begin();
	rd->compute_list_bind_compute_pipeline(l, sh.voxel_unpack_pipeline);
	rd->compute_list_bind_uniform_set(l, voxel_unpack_set0, 0);
	rd->compute_list_set_push_constant(l, &pc, sizeof(pc));
	rd->compute_list_dispatch(l, g, g, g);
	rd->compute_list_end();
}

void RadianceCascade::dispatch_inject() {
	// Inject direct light from light_buffer into the level-0 voxel radiance over the
	// whole grid. The shader marches the SDF toward each light for visibility, so the
	// SDF must be built first.
	RadianceCascadeShaders &sh = *gi->rc_shader;
	RCInjectSlabPushConstant pc = {};
	pc.light_count = light_count;
	pc.vox_origin[0] = vox_origin.x;
	pc.vox_origin[1] = vox_origin.y;
	pc.vox_origin[2] = vox_origin.z;
	pc.voxel_size = vox_extent.x / float(vox_res);
	pc.blend_alpha = 1.0f;
	pc.slab_lo[0] = 0;
	pc.slab_lo[1] = 0;
	pc.slab_lo[2] = 0;
	pc.res = (uint32_t)vox_res;
	pc.slab_dim[0] = vox_res;
	pc.slab_dim[1] = vox_res;
	pc.slab_dim[2] = vox_res;
	pc.phase[0] = vox_phase.x;
	pc.phase[1] = vox_phase.y;
	pc.phase[2] = vox_phase.z;
	const uint32_t g = ((uint32_t)vox_res + 3u) / 4u;
	RD::ComputeListID l = rd->compute_list_begin();
	rd->compute_list_bind_compute_pipeline(l, sh.voxel_inject_pipeline);
	rd->compute_list_bind_uniform_set(l, inject_set0, 0);
	rd->compute_list_set_push_constant(l, &pc, sizeof(pc));
	rd->compute_list_dispatch(l, g, g, g);
	rd->compute_list_end();
}

void RadianceCascade::dispatch_voxel_mips() {
	// Build the six-axis anisotropic radiance mip chain from the level-0 grid. Each
	// level reads the previous (finer) one, so the loop runs in order. The cone trace
	// samples these to keep occlusion direction-aware. Transient set per level.
	RadianceCascadeShaders &sh = *gi->rc_shader;
	const RID shader_rid = sh.voxel_mip_aniso.version_get_shader(sh.voxel_mip_aniso_shader, 0);
	for (int L = 0; L < aniso_levels; L++) {
		const uint32_t dst_res = (uint32_t)(vox_res >> (L + 1));
		Vector<RD::Uniform> u;
		for (int dir = 0; dir < 6; dir++) { // dst views 0..5
			RD::Uniform ud;
			ud.uniform_type = RD::UNIFORM_TYPE_IMAGE;
			ud.binding = dir;
			ud.append_id(aniso_views[dir][L]);
			u.push_back(ud);
		}
		{ // src grid (binding 6): only read when src_is_aniso == 0 (L == 0)
			RD::Uniform us;
			us.uniform_type = RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE;
			us.binding = 6;
			us.append_id(voxel_sampler);
			us.append_id(voxel_tex);
			u.push_back(us);
		}
		for (int dir = 0; dir < 6; dir++) { // src aniso 7..12 = previous level
			RD::Uniform us;
			us.uniform_type = RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE;
			us.binding = 7 + dir;
			us.append_id(voxel_sampler);
			us.append_id(aniso_views[dir][L == 0 ? 0 : (L - 1)]);
			u.push_back(us);
		}
		RID set = rd->uniform_set_create(u, shader_rid, 0);

		RCMipAnisoPushConstant pc = {};
		pc.dst_res = dst_res;
		pc.src_is_aniso = (L == 0) ? 0u : 1u;
		const uint32_t g = (dst_res + 3u) / 4u;
		RD::ComputeListID l = rd->compute_list_begin();
		rd->compute_list_bind_compute_pipeline(l, sh.voxel_mip_aniso_pipeline);
		rd->compute_list_bind_uniform_set(l, set, 0);
		rd->compute_list_set_push_constant(l, &pc, sizeof(pc));
		rd->compute_list_dispatch(l, g, g, g);
		rd->compute_list_end();
		rd->free_rid(set);
	}
}

void RadianceCascade::dispatch_emission_mips() {
	// Isotropic mean mip of the emission grid. Level L reads view[L-1], writes view[L].
	RadianceCascadeShaders &sh = *gi->rc_shader;
	const RID shader_rid = sh.voxel_emission_mip.version_get_shader(sh.voxel_emission_mip_shader, 0);
	for (int L = 1; L < vox_mip_levels; L++) {
		const uint32_t dst_res = (uint32_t)(vox_res >> L);
		Vector<RD::Uniform> u;
		RD::Uniform us;
		us.uniform_type = RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE;
		us.binding = 0;
		us.append_id(voxel_sampler);
		us.append_id(emission_mip_views[L - 1]);
		u.push_back(us);
		RD::Uniform ud;
		ud.uniform_type = RD::UNIFORM_TYPE_IMAGE;
		ud.binding = 1;
		ud.append_id(emission_mip_views[L]);
		u.push_back(ud);
		RID set = rd->uniform_set_create(u, shader_rid, 0);

		RCVoxelMipPushConstant pc = {};
		pc.dst_res = dst_res;
		const uint32_t g = (dst_res + 3u) / 4u;
		RD::ComputeListID l = rd->compute_list_begin();
		rd->compute_list_bind_compute_pipeline(l, sh.voxel_emission_mip_pipeline);
		rd->compute_list_bind_uniform_set(l, set, 0);
		rd->compute_list_set_push_constant(l, &pc, sizeof(pc));
		rd->compute_list_dispatch(l, g, g, g);
		rd->compute_list_end();
		rd->free_rid(set);
	}
}
void RadianceCascade::dispatch_voxel_debug() {}
void RadianceCascade::dispatch_dynamic_voxelize() {}
void RadianceCascade::dispatch_dyn_occ_temporal() {}

/* VOXEL SCENE / SDF */

void RadianceCascade::bake_voxels() {
	// Light the freshly-unpacked grid for the trace: distance field first (inject needs
	// it for sun visibility, and the trace uses it to skip empty space), then inject
	// direct light into voxel radiance, then the aniso + emission mip chains the cone
	// trace samples.
	build_sdf();
	dispatch_inject();
	dispatch_voxel_mips();
	dispatch_emission_mips();
}

void RadianceCascade::build_sdf() {
	// One-shot jump flood of the half-res distance field: seed from occupancy (mode 0),
	// flood with halving step sizes R/2..1 (mode 1, ping-pong), finalize (mode 2).
	RadianceCascadeShaders &sh = *gi->rc_shader;
	const int R = vox_res / 2;
	const uint32_t g = (uint32_t)((R + 3) / 4);
	auto run = [&](RID p_set, uint32_t p_mode, int p_step) {
		RCSdfPushConstant pc = {};
		pc.mode = p_mode;
		pc.step = p_step;
		pc.res = (uint32_t)R;
		pc.phase[0] = vox_phase.x / 2;
		pc.phase[1] = vox_phase.y / 2;
		pc.phase[2] = vox_phase.z / 2;
		RD::ComputeListID l = rd->compute_list_begin();
		rd->compute_list_bind_compute_pipeline(l, sh.voxel_sdf_pipeline);
		rd->compute_list_bind_uniform_set(l, p_set, 0);
		rd->compute_list_set_push_constant(l, &pc, sizeof(pc));
		rd->compute_list_dispatch(l, g, g, g);
		rd->compute_list_end();
	};
	run(sdf_set_write_a, 0u, 0);
	bool seed_in_a = true;
	for (int step = R / 2; step >= 1; step >>= 1) {
		run(seed_in_a ? sdf_set_write_b : sdf_set_write_a, 1u, step);
		seed_in_a = !seed_in_a;
	}
	run(seed_in_a ? sdf_set_write_b : sdf_set_write_a, 2u, 0);
}
