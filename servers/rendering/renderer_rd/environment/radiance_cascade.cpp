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
#include "servers/rendering/renderer_rd/storage_rd/render_scene_buffers_rd.h"
#include "servers/rendering/renderer_rd/storage_rd/render_scene_data_rd.h"
#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"
#include "servers/rendering/rendering_server_globals.h"
#include "servers/rendering/storage/utilities.h"

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
	{ // patch_add has two variants: 0 = near (seed L0 occupancy image), 1 = RC_FAR_SEED (seed a coarse clip level)
		Vector<String> variants;
		variants.push_back("");
		variants.push_back("\n#define RC_FAR_SEED\n");
		patch_add.initialize(variants);
		patch_add_shader = patch_add.version_create();
		patch_add_pipeline = rd->compute_pipeline_create(patch_add.version_get_shader(patch_add_shader, 0));
		patch_add_coarse_pipeline = rd->compute_pipeline_create(patch_add.version_get_shader(patch_add_shader, 1));
	}
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
	if (patch_add_coarse_pipeline.is_valid()) {
		rd->free_rid(patch_add_coarse_pipeline);
		patch_add_coarse_pipeline = RID();
	}
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

void RadianceCascade::configure(RenderSceneBuffersRD *p_render_buffers) {
	// The render buffers call this whenever the viewport is (re)configured -- notably an
	// editor resize. Our screen-sized resources (irradiance chain, debug target) and the
	// cached screen_size the gather reconstructs world position from would otherwise stay
	// at the old dimensions, skewing the GI. Re-create at the new internal size. Only valid
	// once create() has run (gi set); the first-time allocation still comes through create().
	if (gi == nullptr) {
		return;
	}
	const Size2i new_size = p_render_buffers->get_internal_size();
	if (new_size.x == 0 || new_size.y == 0) {
		return;
	}
	free_resources();
	create(gi, new_size);
}

void RadianceCascade::free_resources() {
	// Release every per-viewport RID create() allocated. Idempotent (guards + nulls), so it
	// doubles as the resize path: the render buffers call free_data() then configure() when
	// the viewport is reconfigured, and configure() re-runs create() at the new size. The
	// frame_* RIDs are render-buffer-owned (never ours to free), and the five per-frame sets
	// live in UniformSetCacheRD (it owns their lifetime) -- just drop our stale handles.
	RenderingDevice *d = RD::get_singleton();
	auto fr = [&](RID &r) {
		if (r.is_valid()) {
			d->free_rid(r);
			r = RID();
		}
	};

	// Uniform sets MUST be freed before the textures/buffers they reference: freeing a
	// resource auto-frees every uniform set that bound it, so freeing the resource first
	// would leave us double-freeing an already-reclaimed set ("free invalid ID"). So tear
	// down in dependency order -- sets, then the shared views, then the resources.

	// Static (set 0 / set 2) uniform sets.
	fr(patch_add_set0);
	for (int L = 0; L < MAX_CLIP; L++) {
		fr(clip_add_set[L]);
	}
	fr(patch_clear_set0);
	fr(patch_rebuild_set0);
	fr(patch_lookup_set0);
	fr(patch_indirect_set0);
	fr(patch_trace_set0);
	fr(patch_gather_set0);
	fr(patch_merge_set0);
	fr(patch_neighbours_set0);
	fr(patch_reduce_set0);
	fr(voxelize_set0);
	fr(slab_clear_set);
	fr(dyn_voxelize_set0);
	fr(dyn_occ_temporal_set0);
	fr(trace_voxel_set2);
	fr(voxel_debug_set0);
	fr(inject_set0);
	fr(voxel_unpack_set0);
	fr(sdf_set_write_a);
	fr(sdf_set_write_b);
	fr(atrous_set0_h2s);
	fr(atrous_set0_s2h);
	fr(upsample_set2);
	fr(composite_set0);
	fr(composite_set1);
	for (int L = 0; L < MAX_CLIP; L++) {
		fr(voxel_debug_clip_set[L]);
		fr(clip_voxelize_set[L]);
		fr(clip_inject_set[L]);
		fr(clip_slab_clear_set[L]);
		fr(clip_unpack_set[L]);
	}

	// Cache-owned per-frame sets: drop our handles, don't free (UniformSetCacheRD owns them).
	patch_add_set1 = RID();
	patch_lookup_set1 = RID();
	atrous_set1 = RID();
	upsample_set0 = RID();
	upsample_set1 = RID();

	// Shared texture slices, before their parent textures (voxel_aniso / voxel_emission).
	for (int dir = 0; dir < 6; dir++) {
		for (RID &v : aniso_views[dir]) {
			fr(v);
		}
		aniso_views[dir].clear();
	}
	for (RID &v : emission_mip_views) {
		fr(v);
	}
	emission_mip_views.clear();

	// Camera / cascade table / probe store buffers.
	fr(camera_ubo);
	fr(cascade_buffer);
	fr(patch_buckets);
	fr(patch_alloc);
	fr(patch_keys);
	fr(patch_world);
	fr(patch_live);
	fr(patch_freelist);
	fr(patch_alloc_state);
	fr(probe_radiance);
	fr(probe_rad_tag);
	fr(probe_last_seen);
	fr(patch_neighbours);
	fr(patch_indirect_buffer);
	fr(reduced_radiance);
	fr(probe_inspect_buffer);

	// Voxel grid + render targets + mips.
	fr(voxel_tex);
	fr(voxel_albedo);
	fr(voxel_normal);
	fr(voxel_emission);
	fr(render_albedo);
	fr(render_emission);
	fr(render_emission_aniso);
	fr(render_geom_facing);
	for (int dir = 0; dir < 6; dir++) {
		fr(voxel_aniso[dir]);
	}
	fr(dyn_occ);
	fr(dyn_occ_acc);
	fr(occ_tex);

	// SDF + clip grids + buffers.
	fr(sdf_tex);
	fr(sdf_seed_a);
	fr(sdf_seed_b);
	for (int L = 0; L < MAX_CLIP; L++) {
		fr(clip_grid[L]);
	}
	fr(clip_params_ubo);
	fr(dummy_clip_tex);
	fr(clip_albedo);
	fr(clip_normal);
	fr(clip_emission);
	fr(trace_params_ubo);
	fr(light_buffer);
	fr(tri_buffer);
	fr(dyn_tri_buffer);

	// Screen-space irradiance chain + debug + dummies.
	fr(irradiance_tex);
	fr(irradiance_half);
	fr(irradiance_half_b);
	fr(debug_tex);
	fr(dummy_color_tex);
	fr(dummy_albedo_tex);

	// Samplers.
	fr(voxel_sampler);
	fr(voxel_linear_sampler);
	fr(point_sampler);
	fr(depth_sampler);
	fr(normal_sampler);
	fr(color_sampler);
	fr(albedo_sampler);
}

void RadianceCascade::create(GI *p_gi, const Size2i &p_size) {
	gi = p_gi;
	rd = RD::get_singleton();
	screen_size = p_size;
	half_size = Size2i((p_size.x + 1) / 2, (p_size.y + 1) / 2);

	// Fresh allocation -> fresh (empty) grid + probe pool, so reset the transient state too.
	// create() is reused as the resize path (configure() frees then re-creates), and the
	// object persists across that, so these can't rely on member initializers.
	voxel_dirty = true;
	pending_shells.clear();
	sdf_pass = -1;
	sdf_built_once = false;
	clip_origins_inited = false;

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
	occ_tex = make_tex(RD::DATA_FORMAT_R8_UNORM, RD::TEXTURE_TYPE_3D, vox_res, vox_res, vox_res, 1, usage_occ); // static occupancy for the seed pass
	rd->texture_clear(occ_tex, Color(0, 0, 0, 0), 0, 1, 0, 1);
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
	patch_alloc = filled_buffer(sizeof(uint32_t) * MAX_CASCADES, 0); // zeroed: pre-clear reads must not see garbage
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
	{ // ZERO-INITIALIZED indirect args. The indirect-build → dispatch_indirect barrier in dispatch_patch_trace
		// does not cover INDIRECT_COMMAND_READ, so the very first dispatch_indirect reads this buffer's INITIAL
		// contents (before the build's write is visible). Uninitialized garbage there (e.g. 1.8 B) → ~28 M
		// workgroups → instant GPU hang on the load frame. Zeroed → 0 groups → a harmless skipped first trace.
		const uint32_t isz = sizeof(uint32_t) * 3 * 2 * MAX_CASCADES;
		Vector<uint8_t> iz;
		iz.resize(isz);
		memset(iz.ptrw(), 0, isz);
		patch_indirect_buffer = rd->storage_buffer_create(isz, Span<uint8_t>(iz.ptr(), iz.size()), RD::STORAGE_BUFFER_USAGE_DISPATCH_INDIRECT);
	}

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

void RadianceCascade::process(RenderDataRD *p_render_data, RID p_depth, RID p_normal_roughness, RID p_color, RID p_ambient) {
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
	frame_ambient = p_ambient;
	rebuild_per_frame_sets();
	if (patch_lookup_set1.is_null()) {
		return; // no depth this frame (lookup/gather composite to screen still needs it; ADD does not)
	}

	// The renderer just rasterized + unpacked the shell(s) that scrolled in (a thin band during motion, the
	// whole grid on first frame / teleport) into the grid's occupancy. Re-light ONLY those cells here — the
	// inject's per-cell sun-march is the costly part, so keeping it shell-local is what makes streaming cheap
	// (re-lighting the WHOLE grid every frame roughly halved fps). BUT build the SDF FULLY first: the old
	// localized build_sdf_region reflooded only the shell + a margin and min()-accumulated stale near-
	// occluders, so the cone/inject saw WRONG shadows that differed by approach direction → the same spot
	// looked lit or dark depending on which side you came from. A full reflood runs under ONE toroidal phase
	// and is always correct, so each cell is injected ONCE with a correct SDF and stays correct (static scene)
	// → position-independent, no per-frame full re-light. SDF→inject is one ordered chain (inject reads the
	// freshly built sdf_tex). Standing still = no scroll = no work; the last (correct) inject persists.
	const bool scrolled = !pending_shells.is_empty();
	const uint32_t INJECT_AMORTIZE = 8u; // full grid re-lit every 8 frames (~imperceptible settle)
	if (scrolled) {
		// Localized SDF on scroll (full only on first bake / teleport): cheap, no per-scroll reflood spike.
		// The SDF feeds the inject's sun/light visibility (window-relative march) and the cone trace.
		if (voxel_bake_full || !sdf_built_once) {
			build_sdf();
		} else {
			for (const VoxelShell &s : pending_shells) {
				build_sdf_region(s.lo / 2, (s.dim + Vector3i(1, 1, 1)) / 2);
			}
		}
		sdf_built_once = true;
		for (const VoxelShell &s : pending_shells) {
			dispatch_inject(s.lo, s.dim); // immediate light for the newly-scrolled cells (current window)
		}
		// Capture the scrolled-in regions for the probe-seed pass (occupancy now fresh). A FULL bake = the
		// whole grid as one shell; seeding it in one dispatch is a ~16.7 M-thread atomic storm that hangs the
		// GPU, so route it to the amortized Y-slab seeder (seed_full_*); thin per-scroll shells seed in one go.
		if (voxel_bake_full) {
			seed_full_pending = true;
			seed_full_y = 0;
			seed_shells.clear();
		} else {
			seed_shells = pending_shells;
		}
		pending_shells.clear();
	}

	// AMORTIZED full re-light: re-inject one rotating Y-slab of the grid each frame so EVERY cell refreshes
	// with the CURRENT (camera-centred) window within INJECT_AMORTIZE frames. The per-shell inject above lights
	// a cell correctly ONCE (at scroll-in), but its sun visibility is window-relative (rc_voxel_inject marches
	// the SDF only to the window edge), so leaving that result frozen made the same spot look lit or dark by
	// approach direction. Re-applying it with the current window un-freezes it — the consistency full-relight
	// gave, at ~1/N the cost. The L0 grid is refreshed EVERY frame, so the NEAR GI (which the floor gathers
	// from L0 directly) is always consistent. Needs the SDF built (above, on scroll / at bake).
	if (sdf_built_once) {
		const int slab = (vox_res + int(INJECT_AMORTIZE) - 1) / int(INJECT_AMORTIZE);
		const int y0 = int(frame_index % INJECT_AMORTIZE) * slab;
		const int dy = MIN(slab, vox_res - y0);
		if (dy > 0) {
			dispatch_inject(Vector3i(0, y0, 0), Vector3i(vox_res, dy, vox_res));
		}
	}

	// Radiance mip chain: rebuild on scroll (leading-edge geometry changed) OR once per amortize cycle (the
	// rotating re-light has refreshed the whole grid by then). Throttling the standstill rebuild is what keeps
	// the fps — the mips feed only the FAR/coarse cone samples (near GI is L0-direct), so a few-frame lag is
	// invisible. Emission mips only on scroll: the emission grid changes only when geometry is (re)voxelized,
	// never from the inject, so rebuilding it every frame was pure waste (the bulk of the lost fps).
	if (scrolled || (sdf_built_once && (frame_index % INJECT_AMORTIZE) == 0u)) {
		dispatch_voxel_mips();
	}
	if (scrolled) {
		dispatch_emission_mips();
	}

	// Coarse clipmap: the renderer rasterized + unpacked this frame's one round-robin level into
	// clip_grid[clip_active] (occupancy) + clip scratch. Light its scrolled-in shells -- sun +
	// positional, no SDF march (level 0 carries the sharp shadows). No mips: the trace samples the
	// coarse radiance directly for long range. One level per frame keeps this cheap.
	if (clip_active >= 1 && !clip_pending[clip_active].is_empty()) {
		for (const VoxelShell &s : clip_pending[clip_active]) {
			dispatch_clip_inject(clip_active, s.lo, s.dim);
		}
		clip_pending[clip_active].clear();
	}

	// Per-frame probe chain. A draw-command label groups it for GPU debuggers (RenderDoc/
	// Nsight); the per-pass RENDER_TIMESTAMPs feed the engine's built-in visual profiler --
	// together these replace the GDExtension's bespoke gpu_profile capture. rc_add and
	// rc_upsample are the known hot passes, so they get their own markers.
	RD::get_singleton()->draw_command_begin_label("Radiance Cascades GI");
	dispatch_patch_clear();
	dispatch_patch_rebuild();
	RENDER_TIMESTAMP("RC Add Probes");
	dispatch_patch_add();
	dispatch_patch_add_coarse(); // seed coarse cascades 1..N from their clip levels' occupancy
	RENDER_TIMESTAMP("RC Trace");
	dispatch_patch_trace();
	dispatch_patch_neighbours();
	dispatch_patch_merge();
	dispatch_patch_gather();
	dispatch_irradiance_atrous();
	RENDER_TIMESTAMP("RC Upsample");
	dispatch_irradiance_upsample();
	// The upsample wrote the GI straight into RB_TEX_AMBIENT (frame_ambient); the forward
	// shader consumes it during the opaque pass and multiplies by real albedo.
	RD::get_singleton()->draw_command_end_label();
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
	// Unified spatial clipmap: cascade c covers extent vox_extent*2^c at spacing spacing0*2^c, so every
	// cascade spans the SAME 256^3 cell grid (extent/spacing is constant) -- fine near, coarse far, seeded
	// from voxel level c. Coarse cascades cover far more world per probe, so keep generous but tapering pcaps.
	const uint32_t oct[MAX_CASCADES] = { 4, 4, 8, 8, 8 };
	const uint32_t pcap[MAX_CASCADES] = { 1u << 20, 1u << 19, 1u << 17, 1u << 16, 1u << 15 };
	const float spacing0 = 0.25;

	uint32_t boff = 0, poff = 0, roff = 0;
	float t = 0.0;
	for (uint32_t c = 0; c < MAX_CASCADES; c++) {
		RCCascadeDesc &cd = cascades[c];
		float kc = Math::pow(cascade_scale, (float)c);

		cd.oct_res = oct[c];
		cd.dirs = oct[c] * oct[c];
		cd.aperture = 2.0 / float(oct[c]);

		cd.spacing = spacing0 * kc * dist_mult;
		float len = ray0 * kc * interval_factor * step_mult;
		cd.t_start = t;
		// Ray shells tile [0, far): each cascade picks up where the finer one stopped, so the merge folds
		// them into one field. The interval scales 2x per cascade (penumbra-correct for the 2x spacing).
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

AABB RadianceCascade::voxel_shell_bounds(int p_i) const {
	const float vsize = vox_extent.x / float(vox_res);
	const VoxelShell &s = pending_shells[p_i];
	return AABB(vox_origin + Vector3(s.lo) * vsize, Vector3(s.dim) * vsize);
}

void RadianceCascade::scroll_to(const Vector3 &p_cam_origin) {
	// Snap the grid so it surrounds the camera in whole-voxel steps and record the thin
	// shell(s) that scrolled into view since last frame. Whole-voxel snapping keeps the
	// toroidal phase exact, so the unchanged cells stay valid -- only the exposed band needs
	// re-voxelizing. First frame / teleport / external dirty = the whole grid as one shell.
	pending_shells.clear();

	// Continuous per-frame streaming: re-voxelize the thin band that scrolled into view every
	// frame the grid moves a whole voxel. This used to be gated behind a dead-zone (only re-bake
	// after roaming ~22 m) because the voxelize is an SDFGI-style main-pipeline rasterization that
	// corrupted the opaque pass when run per frame. That's resolved by WHERE the renderer runs it:
	// the thin per-scroll shells are rasterized at the hoisted pre-opaque hook (bounded submission,
	// correct ordering -> no corruption, no hang), and only the rare full first-frame/teleport bake
	// runs at the post-opaque hook. So the gate is gone and the grid follows the camera smoothly.
	const float vsize = vox_extent.x / float(vox_res);
	const int R = vox_res;

	auto ovn_of = [&](const Vector3 &o) {
		return Vector3i((int)Math::floor(o.x / vsize), (int)Math::floor(o.y / vsize), (int)Math::floor(o.z / vsize));
	};

	const Vector3i old_ovn = ovn_of(vox_origin);
	const Vector3 want_origin = p_cam_origin - vox_extent * 0.5f;
	const Vector3i new_ovn = ovn_of(want_origin);
	const Vector3i delta = new_ovn - old_ovn;
	origin_voxel = new_ovn; // absolute grid min corner in voxels (the ADD seed pass needs it)

	const bool full = voxel_dirty || Math::abs(delta.x) >= R || Math::abs(delta.y) >= R || Math::abs(delta.z) >= R;
	voxel_bake_full = full; // tells the renderer which hook to voxelize this frame's bake at

	if (!full && delta == Vector3i()) {
		return; // no whole-voxel movement this frame -> nothing to re-voxelize
	}

	// Commit the new grid placement (snapped to the voxel lattice) + toroidal phase.
	vox_origin = Vector3(new_ovn) * vsize;
	vox_phase = Vector3i(((new_ovn.x % R) + R) % R, ((new_ovn.y % R) + R) % R, ((new_ovn.z % R) + R) % R);
	update_trace_params();
	voxel_dirty = false;

	if (full) {
		pending_shells.push_back({ Vector3i(), Vector3i(R, R, R) }); // whole grid
		return;
	}

	// One shell per moved axis: the band of |delta| voxels newly exposed at the leading edge
	// (render-grid coords, relative to the new origin). Other axes span the full grid.
	for (int axis = 0; axis < 3; axis++) {
		const int d = delta[axis];
		if (d == 0) {
			continue;
		}
		VoxelShell s;
		s.lo = Vector3i();
		s.dim = Vector3i(R, R, R);
		s.dim[axis] = Math::abs(d);
		s.lo[axis] = (d > 0) ? (R - Math::abs(d)) : 0;
		pending_shells.push_back(s);
	}
}

int RadianceCascade::clip_step(const Vector3 &p_cam_origin) {
	// Round-robin: voxelize ONE coarse level per frame. Coarse levels change slowly (2x the voxel
	// size per level, up to 4 m), so a 1-in-(clip_levels-1) update rate is invisible, and it lets
	// every coarse level share the single packed/clip scratch (no per-level VRAM). Returns the
	// level the renderer should rasterize this frame, or -1 if nothing scrolled at that level.
	clip_active = -1;
	if (clip_levels <= 1) {
		return -1;
	}
	clip_rr_cursor++;
	if (clip_rr_cursor >= clip_levels) {
		clip_rr_cursor = 1;
	}
	const int L = clip_rr_cursor;
	clip_scroll(L, p_cam_origin);
	if (!clip_pending[L].is_empty()) {
		clip_active = L;
		update_trace_params(); // publish the level's new origin to the trace UBO
	}
	return clip_active;
}

void RadianceCascade::clip_scroll(int p_level, const Vector3 &p_cam_origin) {
	// Same toroidal snap + leading-edge shell math as scroll_to, at this level's coarser voxel
	// size / larger extent. First touch of a level (level_dirty) or a teleport = the whole grid.
	clip_pending[p_level].clear();
	const int R = vox_res;
	const float vsize = (vox_extent.x / float(R)) * float(1 << p_level);
	const float extent = vsize * float(R);
	auto ovn_of = [&](const Vector3 &o) {
		return Vector3i((int)Math::floor(o.x / vsize), (int)Math::floor(o.y / vsize), (int)Math::floor(o.z / vsize));
	};
	const Vector3i old_ovn = ovn_of(clip_origin[p_level]);
	const Vector3 want_origin = p_cam_origin - Vector3(extent, extent, extent) * 0.5f;
	const Vector3i new_ovn = ovn_of(want_origin);
	const Vector3i delta = new_ovn - old_ovn;
	const bool full = level_dirty[p_level] || Math::abs(delta.x) >= R || Math::abs(delta.y) >= R || Math::abs(delta.z) >= R;
	clip_bake_full[p_level] = full;
	if (!full && delta == Vector3i()) {
		return; // no whole-voxel movement at this level this frame
	}
	clip_origin[p_level] = Vector3(new_ovn) * vsize;
	clip_phase[p_level] = Vector3i(((new_ovn.x % R) + R) % R, ((new_ovn.y % R) + R) % R, ((new_ovn.z % R) + R) % R);
	level_dirty[p_level] = false;
	if (full) {
		clip_pending[p_level].push_back({ Vector3i(), Vector3i(R, R, R) });
		return;
	}
	for (int axis = 0; axis < 3; axis++) {
		const int d = delta[axis];
		if (d == 0) {
			continue;
		}
		VoxelShell s;
		s.lo = Vector3i();
		s.dim = Vector3i(R, R, R);
		s.dim[axis] = Math::abs(d);
		s.lo[axis] = (d > 0) ? (R - Math::abs(d)) : 0;
		clip_pending[p_level].push_back(s);
	}
}

AABB RadianceCascade::clip_shell_bounds(int p_i) const {
	const int L = clip_active;
	const float vsize = (vox_extent.x / float(vox_res)) * float(1 << L);
	const VoxelShell &s = clip_pending[L][p_i];
	return AABB(clip_origin[L] + Vector3(s.lo) * vsize, Vector3(s.dim) * vsize);
}

void RadianceCascade::dispatch_clip_unpack(int p_level, const Vector3i &p_lo, const Vector3i &p_dim) {
	// Unpack the shared packed render targets (just rasterized at this level's extent) into
	// clip_grid[p_level] (occupancy in .a) + clip scratch (albedo/normal/emission). Same shader as
	// the L0 unpack; the toroidal write uses this level's phase.
	RadianceCascadeShaders &sh = *gi->rc_shader;
	RCVoxelUnpackPushConstant pc = {};
	pc.phase[0] = clip_phase[p_level].x;
	pc.phase[1] = clip_phase[p_level].y;
	pc.phase[2] = clip_phase[p_level].z;
	pc.res = (uint32_t)vox_res;
	pc.slab_lo[0] = p_lo.x;
	pc.slab_lo[1] = p_lo.y;
	pc.slab_lo[2] = p_lo.z;
	pc.slab_dim[0] = p_dim.x;
	pc.slab_dim[1] = p_dim.y;
	pc.slab_dim[2] = p_dim.z;
	RD::ComputeListID l = rd->compute_list_begin();
	rd->compute_list_bind_compute_pipeline(l, sh.voxel_unpack_pipeline);
	rd->compute_list_bind_uniform_set(l, clip_unpack_set[p_level], 0);
	rd->compute_list_set_push_constant(l, &pc, sizeof(pc));
	rd->compute_list_dispatch(l, ((uint32_t)p_dim.x + 3u) / 4u, ((uint32_t)p_dim.y + 3u) / 4u, ((uint32_t)p_dim.z + 3u) / 4u);
	rd->compute_list_end();
}

void RadianceCascade::dispatch_clip_inject(int p_level, const Vector3i &p_lo, const Vector3i &p_dim) {
	// Light a coarse level's shell: sun (from sun_dir/sun_color) + positional lights, no SDF march
	// (vis from coarse occupancy; level 0 carries the sharp shadows). Indexes by absolute world
	// voxel (slab_lo = level origin voxel + p_lo) so the toroidal cell matches the unpack's.
	RadianceCascadeShaders &sh = *gi->rc_shader;
	const int R = vox_res;
	const float vsize = (vox_extent.x / float(R)) * float(1 << p_level);
	const Vector3i ovn((int)Math::floor(clip_origin[p_level].x / vsize), (int)Math::floor(clip_origin[p_level].y / vsize), (int)Math::floor(clip_origin[p_level].z / vsize));
	RCClipInjectSlabPushConstant pc = {};
	pc.sun_dir[0] = sun_dir.x;
	pc.sun_dir[1] = sun_dir.y;
	pc.sun_dir[2] = sun_dir.z;
	pc.blend_alpha = 1.0f;
	// sun_color slot now carries the SKY ambient (the sun itself comes from the light buffer in the
	// shader). Distant coarse surfaces bake this as flat skylight so they aren't pure black.
	pc.sun_color[0] = sky_color.x;
	pc.sun_color[1] = sky_color.y;
	pc.sun_color[2] = sky_color.z;
	pc.voxel_size = vsize;
	pc.slab_lo[0] = ovn.x + p_lo.x;
	pc.slab_lo[1] = ovn.y + p_lo.y;
	pc.slab_lo[2] = ovn.z + p_lo.z;
	pc.res = (uint32_t)R;
	pc.slab_dim[0] = p_dim.x;
	pc.slab_dim[1] = p_dim.y;
	pc.slab_dim[2] = p_dim.z;
	pc.light_count = light_count;
	pc.phase[0] = clip_phase[p_level].x;
	pc.phase[1] = clip_phase[p_level].y;
	pc.phase[2] = clip_phase[p_level].z;
	pc.level = (uint32_t)p_level;
	RD::ComputeListID l = rd->compute_list_begin();
	rd->compute_list_bind_compute_pipeline(l, sh.clip_inject_pipeline);
	rd->compute_list_bind_uniform_set(l, clip_inject_set[p_level], 0);
	rd->compute_list_set_push_constant(l, &pc, sizeof(pc));
	rd->compute_list_dispatch(l, ((uint32_t)p_dim.x + 3u) / 4u, ((uint32_t)p_dim.y + 3u) / 4u, ((uint32_t)p_dim.z + 3u) / 4u);
	rd->compute_list_end();
}

void RadianceCascade::unpack_voxels() {
	for (const VoxelShell &s : pending_shells) {
		dispatch_voxel_unpack(s.lo, s.dim);
	}
}

void RadianceCascade::unpack_clip() {
	if (clip_active < 1) {
		return;
	}
	for (const VoxelShell &s : clip_pending[clip_active]) {
		dispatch_clip_unpack(clip_active, s.lo, s.dim);
	}
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

	{ // patch add: world seeding from voxel-grid occupancy (b6 = voxel_tex, cascade table b7). ADD appends
		// only its NEW allocs to the live list; REBUILD appends carried-over probes + owns eviction.
		Vector<RD::Uniform> u;
		u.push_back(ssbo(0, patch_buckets));
		u.push_back(ssbo(1, patch_alloc));
		u.push_back(ssbo(2, patch_keys));
		u.push_back(ssbo(3, patch_world));
		u.push_back(ssbo(4, patch_live));
		u.push_back(img(6, occ_tex)); // static occupancy seed source (storage read, GENERAL — no layout transition)
		u.push_back(img(5, voxel_normal)); // voxel normal (storage read, GENERAL) — offset the seed to the surface face
		u.push_back(ssbo(7, cascade_buffer));
		u.push_back(ssbo(8, probe_rad_tag));
		u.push_back(ssbo(10, probe_last_seen));
		u.push_back(ssbo(11, patch_freelist));
		u.push_back(ssbo(12, patch_alloc_state));
		patch_add_set0 = rd->uniform_set_create(u, RC_SHADER(patch_add), 0);
	}

	// Coarse seed sets (RC_FAR_SEED variant 1), one per coarse clip level: same probe buffers, but
	// occupancy comes from clip_grid[L] (b6 = sampler, .a occupancy) and there's no normal grid (b5).
	// cascade L is seeded from voxel level L's occupancy -- the unified clipmap seeding.
	for (int L = 1; L < MAX_CLIP; L++) {
		RID grid = (L < clip_levels && clip_grid[L].is_valid()) ? clip_grid[L] : dummy_clip_tex;
		Vector<RD::Uniform> u;
		u.push_back(ssbo(0, patch_buckets));
		u.push_back(ssbo(1, patch_alloc));
		u.push_back(ssbo(2, patch_keys));
		u.push_back(ssbo(3, patch_world));
		u.push_back(ssbo(4, patch_live));
		u.push_back(tex(6, point_sampler, grid)); // coarse occupancy (.a), sampled toroidally like the trace
		u.push_back(ssbo(7, cascade_buffer));
		u.push_back(ssbo(8, probe_rad_tag));
		u.push_back(ssbo(10, probe_last_seen));
		u.push_back(ssbo(11, patch_freelist));
		u.push_back(ssbo(12, patch_alloc_state));
		clip_add_set[L] = rd->uniform_set_create(u, sh.patch_add.version_get_shader(sh.patch_add_shader, 1), 0);
	}

	{ // patch rebuild (dense pool -> repopulate hashmap + append live list + spatial evict)
		Vector<RD::Uniform> u;
		u.push_back(ssbo(0, patch_buckets));
		u.push_back(ssbo(1, patch_alloc)); // live counter (append target)
		u.push_back(ssbo(2, patch_keys));
		u.push_back(ssbo(3, patch_world)); // probe centers (spatial eviction test)
		u.push_back(ssbo(4, patch_live)); // live list (now built here)
		u.push_back(ssbo(7, cascade_buffer));
		u.push_back(ssbo(8, probe_last_seen));
		u.push_back(ssbo(9, probe_rad_tag)); // bootstrap detect
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
		u.push_back(ubo(5, camera_ubo)); // frustum-limited tracing
		u.push_back(ssbo(6, probe_radiance));
		u.push_back(ssbo(7, cascade_buffer));
		patch_trace_set0 = rd->uniform_set_create(u, RC_SHADER(patch_trace), 0);
	}

	{ // patch merge (neighbour ids precomputed at b9)
		Vector<RD::Uniform> u;
		u.push_back(ssbo(1, patch_alloc));
		u.push_back(ssbo(3, patch_world));
		u.push_back(ssbo(4, patch_live));
		u.push_back(ubo(5, camera_ubo)); // frustum-limited (lockstep with trace)
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

	// NOTE: upsample set0 (binding 1 = the output target) is built per-frame in
	// rebuild_per_frame_sets, since it writes the per-frame RB_TEX_AMBIENT.

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
		u.push_back(ubo(8, clip_params_ubo)); // per-cascade spatial windows for finest-covering selection
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
		u.push_back(img(6, occ_tex)); // static occupancy mirror for the seed pass
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
			u.push_back(tex(5, voxel_linear_sampler, voxel_tex)); // L0 lit radiance (mipped) for the overlap downsample
			u.push_back(ubo(6, clip_params_ubo)); // per-level origin/extent table (reads lvl[0] = L0)
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
		{ // coarse unpack: shared packed render targets -> clip_grid[L] (occupancy) + clip scratch.
			// Reuses rc_voxel_unpack (formats match voxel_*); the SDFGI rasterizer fills the packed
			// targets at the coarse level's extent, this writes them toroidally into the coarse grid.
			Vector<RD::Uniform> u;
			u.push_back(img(0, render_albedo));
			u.push_back(img(1, render_emission));
			u.push_back(img(2, render_geom_facing));
			u.push_back(img(3, clip_grid[L]));
			u.push_back(img(4, clip_albedo));
			u.push_back(img(5, clip_normal));
			u.push_back(img(6, clip_emission));
			clip_unpack_set[L] = rd->uniform_set_create(u, RC_SHADER(voxel_unpack), 0);
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

	// These sets reference this frame's render-buffer textures (depth / normal / ambient),
	// which the engine frees and recreates whenever the viewport is reconfigured (e.g. an
	// editor resize). Freeing such a texture auto-frees the uniform sets that referenced it,
	// so a manually-cached set RID goes stale and double-freeing it spams "free invalid ID"
	// and leaves the dispatch reading garbage. Route them through UniformSetCacheRD instead:
	// it keys by contents, registers an invalidation callback, and owns the lifetime -- we
	// never free these ourselves, and a fresh valid set is returned whenever inputs change.
	UniformSetCacheRD *usc = UniformSetCacheRD::get_singleton();

	auto tex = [](int p_bind, RID p_sampler, RID p_tex) {
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE;
		u.binding = p_bind;
		u.append_id(p_sampler);
		u.append_id(p_tex);
		return u;
	};

	// lookup set1: depth(0) + normal(1) -- also bound by gather
	patch_lookup_set1 = usc->get_cache(sh.patch_lookup.version_get_shader(sh.patch_lookup_shader, 0), 1,
			tex(0, depth_sampler, frame_depth), tex(1, normal_sampler, frame_normal));

	// (ADD no longer uses a per-frame set: it seeds from the persistent voxel-grid occupancy, not depth.)

	{ // upsample set0: half-res irradiance in (b0), RB_TEX_AMBIENT out (b1)
		const RID out = frame_ambient.is_valid() ? frame_ambient : irradiance_tex;
		RD::Uniform o;
		o.uniform_type = RD::UNIFORM_TYPE_IMAGE;
		o.binding = 1;
		o.append_id(out);
		upsample_set0 = usc->get_cache(sh.irradiance_upsample.version_get_shader(sh.irradiance_upsample_shader, 0), 0,
				tex(0, point_sampler, irradiance_half), o);
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
			// The coarse clip_inject takes the sun from its push constant (it skips directional
			// lights in the buffer), so mirror this frame's directional light into sun_dir/sun_color.
			// sun_dir points TOWARD the sun (-shine), so N.L lights up-facing surfaces under it.
			sun_dir = -dir;
			sun_color = Vector3(g.color[0], g.color[1], g.color[2]);
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
	// Repopulate the cleared hashmap from the persistent dense pool (one thread per dense id per
	// cascade) AND append every alive in-window probe to the live list (so the trace updates it this
	// frame regardless of camera facing — view-independent). Probes whose center scrolled OUT of the
	// grid window are evicted to the free-list (spatial eviction; in-range probes are immortal).
	RadianceCascadeShaders &sh = *gi->rc_shader;
	RD::ComputeListID l = rd->compute_list_begin();
	rd->compute_list_bind_compute_pipeline(l, sh.patch_rebuild_pipeline);
	rd->compute_list_bind_uniform_set(l, patch_rebuild_set0, 0);
	for (uint32_t c = 0; c < MAX_CASCADES; c++) {
		RCPatchRebuildPushConstant pc = {};
		pc.frame = frame_index;
		pc.cascade = c;
		// Per-cascade eviction window: cascade c lives in voxel level c's window (extent vox_extent*2^c),
		// so it must evict against THAT -- else a coarse probe, being outside the finer L0 box, is culled
		// the frame after it's seeded. Level 0 = the L0 grid; levels 1..N = the clip windows.
		Vector3 wmin = vox_origin;
		Vector3 wmax = vox_origin + vox_extent;
		if (c >= 1 && (int)c < clip_levels) {
			const float ext = vox_extent.x * float(1 << c);
			wmin = clip_origin[c];
			wmax = wmin + Vector3(ext, ext, ext);
		}
		pc.win_min[0] = wmin.x;
		pc.win_min[1] = wmin.y;
		pc.win_min[2] = wmin.z;
		pc.win_max[0] = wmax.x;
		pc.win_max[1] = wmax.y;
		pc.win_max[2] = wmax.z;
		rd->compute_list_set_push_constant(l, &pc, sizeof(pc));
		rd->compute_list_dispatch(l, (cascades[c].probe_cap + 63u) / 64u, 1, 1);
	}
	rd->compute_list_end();
}

void RadianceCascade::dispatch_patch_add() {
	// World seeding: find-or-allocate probes from voxel-grid OCCUPANCY over the shell(s) that streamed
	// in this frame (their occupancy in voxel_tex is fresh). One thread per shell voxel; occupied cells
	// seed the 8 gather corners for every cascade. View-independent (no screen/depth): a probe exists
	// for an occupied cell whenever that cell is in range, regardless of camera facing. Empty seed_shells
	// (no scroll this frame) → nothing to do; existing in-range probes persist + keep tracing (rebuild).
	if (seed_shells.is_empty() && !seed_full_pending && !sdf_built_once) {
		return; // nothing voxelized yet (the continuous re-seed below runs once occupancy exists)
	}
	const float vsize = vox_extent.x / float(vox_res);
	RadianceCascadeShaders &sh = *gi->rc_shader;
	RD::ComputeListID l = rd->compute_list_begin();
	rd->compute_list_bind_compute_pipeline(l, sh.patch_add_pipeline);
	rd->compute_list_bind_uniform_set(l, patch_add_set0, 0);

	// Seed one window-relative region [lo_rel, lo_rel+dim) of occupied cells (one thread per voxel).
	auto seed_region = [&](const Vector3i &lo_rel, const Vector3i &dim) {
		RCPatchAddPushConstant pc = {};
		pc.seed_lo[0] = origin_voxel.x + lo_rel.x; // absolute world voxel of the region corner
		pc.seed_lo[1] = origin_voxel.y + lo_rel.y;
		pc.seed_lo[2] = origin_voxel.z + lo_rel.z;
		pc.seed_dim[0] = dim.x;
		pc.seed_dim[1] = dim.y;
		pc.seed_dim[2] = dim.z;
		pc.cascade_begin = 0;
		pc.cascade_end = 1; // cascade 0 only (the L0 fine level); coarse cascades 1..N seed from their clip grids
		pc.res = (uint32_t)vox_res;
		pc.voxel_size = vsize;
		pc.frame = frame_index;
		rd->compute_list_set_push_constant(l, &pc, sizeof(pc));
		rd->compute_list_dispatch(l, ((uint32_t)dim.x + 3u) / 4u, ((uint32_t)dim.y + 3u) / 4u, ((uint32_t)dim.z + 3u) / 4u);
	};

	for (const VoxelShell &s : seed_shells) {
		seed_region(s.lo, s.dim); // thin per-scroll shells (small, seed in one go)
	}
	seed_shells.clear();

	// Drain a few slabs of the amortized full-bake seed (spreads the 16.7 M-thread storm over frames).
	if (seed_full_pending) {
		const int SLAB = 4; // voxels in Y per slab (conservative: low per-frame bootstrap-trace load)
		const int SLABS_PER_FRAME = 1; // ~64 frames (~1 s) to fill the 256-tall grid, bottom-up
		for (int k = 0; k < SLABS_PER_FRAME && seed_full_pending; k++) {
			const int dimy = MIN(SLAB, vox_res - seed_full_y);
			seed_region(Vector3i(0, seed_full_y, 0), Vector3i(vox_res, dimy, vox_res));
			seed_full_y += SLAB;
			if (seed_full_y >= vox_res) {
				seed_full_pending = false;
			}
		}
	}

	// CONTINUOUS amortized re-seed: cover one rotating Y-slab of the grid from occupancy EVERY frame, so every
	// occupied cell gets a probe within N frames regardless of scroll/bake history. The one-shot bottom-up
	// seed_full fill restarts at the floor on every full bake (frequent under fast movement) and never climbs
	// to the walls/ceiling, so those stayed unseeded (red in the probe debug) even though their occupancy is
	// correct (visible in the Voxel-Grid view). This self-heals that. Idempotent: an already-seeded cell just
	// re-FINDS its probe (no alloc, no bootstrap-trace); only genuinely new cells allocate. Needs occupancy
	// (sdf_built_once ⇒ the inject has mirrored voxel_tex.a → occ_tex at least once).
	if (sdf_built_once) {
		const int N = 8; // full grid re-seeded every 8 frames (~imperceptible heal latency)
		const int slab = (vox_res + N - 1) / N;
		const int y0 = int(frame_index % uint32_t(N)) * slab;
		const int dy = MIN(slab, vox_res - y0);
		if (dy > 0) {
			seed_region(Vector3i(0, y0, 0), Vector3i(vox_res, dy, vox_res));
		}
	}
	rd->compute_list_end();
}

void RadianceCascade::dispatch_patch_add_coarse() {
	// Unified clipmap seeding for the coarse cascades: cascade L is seeded from voxel level L's occupancy
	// (clip_grid[L]) over a rotating slab of that level's window each frame (amortized, like the L0 re-seed).
	// cascade L's spacing == clip level L's voxel size, so it's one probe per occupied coarse voxel. The
	// per-cascade rebuild evicts probes that scroll out of level L's window.
	if (!sdf_built_once) {
		return; // no occupancy yet
	}
	const int R = vox_res;
	const int N = 8; // whole window re-seeded every 8 frames
	const int slab = (R + N - 1) / N;
	const int y0 = int(frame_index % uint32_t(N)) * slab;
	const int dy = MIN(slab, R - y0);
	if (dy <= 0) {
		return;
	}
	RadianceCascadeShaders &sh = *gi->rc_shader;
	RD::ComputeListID l = rd->compute_list_begin();
	rd->compute_list_bind_compute_pipeline(l, sh.patch_add_coarse_pipeline);

	for (int L = 1; L < (int)MAX_CASCADES && L < clip_levels; L++) {
		if (!clip_grid[L].is_valid() || clip_add_set[L].is_null()) {
			continue;
		}
		const float fvs = (vox_extent.x / float(R)) * float(1 << L); // level L voxel size == cascade L spacing
		const float extent = fvs * float(R);
		const Vector3 origin = clip_origin[L];
		const Vector3i ovn((int)Math::floor(origin.x / fvs), (int)Math::floor(origin.y / fvs), (int)Math::floor(origin.z / fvs));
		rd->compute_list_bind_uniform_set(l, clip_add_set[L], 0);
		RCPatchAddPushConstant pc = {};
		pc.seed_lo[0] = ovn.x;
		pc.seed_lo[1] = ovn.y + y0;
		pc.seed_lo[2] = ovn.z;
		pc.seed_dim[0] = R;
		pc.seed_dim[1] = dy;
		pc.seed_dim[2] = R;
		pc.cascade_begin = (uint32_t)L;
		pc.cascade_end = (uint32_t)L + 1u;
		pc.res = (uint32_t)R;
		pc.voxel_size = fvs;
		pc.frame = frame_index;
		pc.coarse_extent = extent;
		rd->compute_list_set_push_constant(l, &pc, sizeof(pc));
		rd->compute_list_dispatch(l, ((uint32_t)R + 3u) / 4u, ((uint32_t)dy + 3u) / 4u, ((uint32_t)R + 3u) / 4u);
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
			// local_trans=1 for ALL cascades incl. far: skip the from-origin pre-roll. The far probe sits
			// INSIDE its own solid coarse cell, so a from-origin march (local_trans=0) would drive
			// transmittance to ~0 in the first step and return black; instead it measures fresh from
			// t_start (which the cascade table set a couple coarse voxels out to clear the self cell).
			pc.local_transmittance = local_transmittance ? 1u : 0u;
			pc.frame = frame_index;
			pc.amortize_n = eff_amortize;
			pc.probe_amortize = probe_amortize;
			pc.frustum_margin = frustum_margin;
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
	// Every cascade folds into the finer one (c+1 -> c); the top cascade has no parent.
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
	// Fold cascade c+1 into c across the whole clipmap (spatial trilinear in the overlap; c+1 covers 2x
	// the extent so it always contains c's probes). Each cascade ends holding its interval + all coarser
	// continuations; the gather then reads the finest cascade that covers each pixel.
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
		pc.probe_amortize = probe_amortize;
		pc.frustum_margin = frustum_margin;
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
	{ // per-frame set1: depth + normal (NEAREST). Cached -- see rebuild_per_frame_sets.
		RD::Uniform ud;
		ud.uniform_type = RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE;
		ud.binding = 0;
		ud.append_id(point_sampler);
		ud.append_id(frame_depth);
		RD::Uniform un;
		un.uniform_type = RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE;
		un.binding = 1;
		un.append_id(point_sampler);
		un.append_id(frame_normal);
		atrous_set1 = UniformSetCacheRD::get_singleton()->get_cache(sh.irradiance_atrous.version_get_shader(sh.irradiance_atrous_shader, 0), 1, ud, un);
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
	{ // per-frame set1: depth + normal. Cached -- see rebuild_per_frame_sets.
		RD::Uniform ud;
		ud.uniform_type = RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE;
		ud.binding = 0;
		ud.append_id(point_sampler);
		ud.append_id(frame_depth);
		RD::Uniform un;
		un.uniform_type = RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE;
		un.binding = 1;
		un.append_id(point_sampler);
		un.append_id(frame_normal);
		upsample_set1 = UniformSetCacheRD::get_singleton()->get_cache(sh.irradiance_upsample.version_get_shader(sh.irradiance_upsample_shader, 0), 1, ud, un);
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
	pc.gi_intensity = gi_intensity;
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

void RadianceCascade::dispatch_patch_lookup(uint32_t p_debug_kind) {
	// Debug visualization: shade each pixel from the probe field (kind 0 = occupancy/
	// existence, kind 1 = traced radiance) into debug_tex, which the renderer then blits
	// over the final frame. Reads the per-frame depth/normal set built in process().
	RadianceCascadeShaders &sh = *gi->rc_shader;
	RCPatchLookupPushConstant pc = {};
	pc.screen_width = (uint32_t)screen_size.x;
	pc.screen_height = (uint32_t)screen_size.y;
	pc.debug_kind = p_debug_kind;
	pc.cascade = MIN(debug_cascade, MAX_CASCADES - 1u);
	pc.z_near = z_near;
	pc.z_far = z_far;
	pc.sky_color[0] = sky_color.x;
	pc.sky_color[1] = sky_color.y;
	pc.sky_color[2] = sky_color.z;
	RD::ComputeListID l = rd->compute_list_begin();
	rd->compute_list_bind_compute_pipeline(l, sh.patch_lookup_pipeline);
	rd->compute_list_bind_uniform_set(l, patch_lookup_set0, 0);
	rd->compute_list_bind_uniform_set(l, patch_lookup_set1, 1);
	rd->compute_list_set_push_constant(l, &pc, sizeof(pc));
	rd->compute_list_dispatch(l, ((uint32_t)screen_size.x + 7u) / 8u, ((uint32_t)screen_size.y + 7u) / 8u, 1);
	rd->compute_list_end();
}
void RadianceCascade::dispatch_voxel_unpack(const Vector3i &p_lo, const Vector3i &p_dim) {
	// Unpack the packed voxelization targets into the RC voxel grid, over a render-grid
	// region [p_lo, p_lo+p_dim) (the shell that was just rasterized there). One thread/cell;
	// the shader wraps the write into the toroidal grid via the phase.
	RadianceCascadeShaders &sh = *gi->rc_shader;
	RCVoxelUnpackPushConstant pc = {};
	pc.phase[0] = vox_phase.x;
	pc.phase[1] = vox_phase.y;
	pc.phase[2] = vox_phase.z;
	pc.res = (uint32_t)vox_res;
	pc.slab_lo[0] = p_lo.x;
	pc.slab_lo[1] = p_lo.y;
	pc.slab_lo[2] = p_lo.z;
	pc.slab_dim[0] = p_dim.x;
	pc.slab_dim[1] = p_dim.y;
	pc.slab_dim[2] = p_dim.z;
	RD::ComputeListID l = rd->compute_list_begin();
	rd->compute_list_bind_compute_pipeline(l, sh.voxel_unpack_pipeline);
	rd->compute_list_bind_uniform_set(l, voxel_unpack_set0, 0);
	rd->compute_list_set_push_constant(l, &pc, sizeof(pc));
	rd->compute_list_dispatch(l, ((uint32_t)p_dim.x + 3u) / 4u, ((uint32_t)p_dim.y + 3u) / 4u, ((uint32_t)p_dim.z + 3u) / 4u);
	rd->compute_list_end();
}

void RadianceCascade::dispatch_inject(const Vector3i &p_lo, const Vector3i &p_dim) {
	// Inject direct light from light_buffer into the level-0 voxel radiance over a render-grid
	// region [p_lo, p_lo+p_dim) (a freshly-streamed shell, or the whole grid). The shader
	// marches the SDF toward each light for visibility, so the SDF must already be built. It
	// indexes by absolute world voxel (slab_lo = origin voxel + p_lo), which both wraps to the
	// right toroidal cell (== the unpack's cell) and gives the correct world position.
	RadianceCascadeShaders &sh = *gi->rc_shader;
	const float vsize = vox_extent.x / float(vox_res);
	const Vector3i ovn((int)Math::floor(vox_origin.x / vsize), (int)Math::floor(vox_origin.y / vsize), (int)Math::floor(vox_origin.z / vsize));
	RCInjectSlabPushConstant pc = {};
	pc.light_count = light_count;
	pc.vox_origin[0] = vox_origin.x;
	pc.vox_origin[1] = vox_origin.y;
	pc.vox_origin[2] = vox_origin.z;
	pc.voxel_size = vsize;
	pc.blend_alpha = 1.0f;
	pc.slab_lo[0] = ovn.x + p_lo.x;
	pc.slab_lo[1] = ovn.y + p_lo.y;
	pc.slab_lo[2] = ovn.z + p_lo.z;
	pc.res = (uint32_t)vox_res;
	pc.slab_dim[0] = p_dim.x;
	pc.slab_dim[1] = p_dim.y;
	pc.slab_dim[2] = p_dim.z;
	pc.phase[0] = vox_phase.x;
	pc.phase[1] = vox_phase.y;
	pc.phase[2] = vox_phase.z;
	RD::ComputeListID l = rd->compute_list_begin();
	rd->compute_list_bind_compute_pipeline(l, sh.voxel_inject_pipeline);
	rd->compute_list_bind_uniform_set(l, inject_set0, 0);
	rd->compute_list_set_push_constant(l, &pc, sizeof(pc));
	rd->compute_list_dispatch(l, ((uint32_t)p_dim.x + 3u) / 4u, ((uint32_t)p_dim.y + 3u) / 4u, ((uint32_t)p_dim.z + 3u) / 4u);
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
void RadianceCascade::dispatch_debug() {
	// Render the selected debug view into debug_tex (the renderer blits it over the frame afterwards).
	// Runs after process() this frame, so the voxel grid + probe field are populated. View ids match the
	// rc_debug enum, grouped per clipmap level L (0..MAX_CASCADES-1):
	//   0        = Off
	//   1..5     = Voxel L0..L4     -> march voxel level L's grid (occupancy relief + stored rgb)
	//   6..10    = Probe Occ L0..L4 -> probe-id / coverage of cascade L (red = genuine miss)
	//   11..15   = Probe Rad L0..L4 -> gather-preview radiance reconstructed from cascade L's probes
	if (debug_view >= 1 && debug_view <= 5) {
		dispatch_voxel_debug(debug_view - 1);
	} else if (debug_view >= 6 && debug_view <= 10) {
		debug_cascade = (uint32_t)(debug_view - 6);
		dispatch_patch_lookup(0);
	} else if (debug_view >= 11 && debug_view <= 15) {
		debug_cascade = (uint32_t)(debug_view - 11);
		dispatch_patch_lookup(1);
	}
}

void RadianceCascade::dispatch_voxel_debug(int p_level) {
	// Debug visualization: march a primary ray per pixel through a voxel grid and shade the
	// first solid voxel (gradient normal relief + the grid's stored rgb) into debug_tex. p_level
	// 0 = the L0 grid (rgb = emission; lit radiance lives elsewhere). p_level 1..4 = a coarse clip
	// level, whose rgb IS the baked sun+bounce radiance -- so a coarse level reads as GRAY relief
	// if voxelized-but-dark, COLOURED if lit, BLACK if not voxelized. The level's own origin/extent/
	// voxel_size drive the march so it samples the right toroidal cells.
	RadianceCascadeShaders &sh = *gi->rc_shader;
	const bool coarse = p_level >= 1 && p_level < clip_levels && clip_grid[p_level].is_valid();
	const float base = vox_extent.x / float(vox_res);
	const float vsize = coarse ? base * float(1 << p_level) : base;
	const float extent = vsize * float(vox_res);
	const Vector3 origin = coarse ? clip_origin[p_level] : vox_origin;
	RCVoxelDebugPushConstant pc = {};
	pc.sw = (uint32_t)screen_size.x;
	pc.sh = (uint32_t)screen_size.y;
	pc.res = (uint32_t)vox_res;
	pc.max_steps = 512;
	pc.vox_origin[0] = origin.x;
	pc.vox_origin[1] = origin.y;
	pc.vox_origin[2] = origin.z;
	pc.voxel_size = vsize;
	pc.vox_extent[0] = extent;
	pc.vox_extent[1] = extent;
	pc.vox_extent[2] = extent;
	pc.occ_threshold = 0.3f;
	// L0's rgb is emission only (relief carries the geometry); coarse rgb IS the baked sun+sky
	// radiance, which is dim -- boost it and dim the relief so "is it lit / how much" reads clearly.
	pc.radiance_gain = coarse ? 6.0f : 1.0f;
	pc.relief_gain = coarse ? 0.12f : 1.0f;
	RD::ComputeListID l = rd->compute_list_begin();
	rd->compute_list_bind_compute_pipeline(l, sh.voxel_debug_pipeline);
	rd->compute_list_bind_uniform_set(l, coarse ? voxel_debug_clip_set[p_level] : voxel_debug_set0, 0);
	rd->compute_list_set_push_constant(l, &pc, sizeof(pc));
	rd->compute_list_dispatch(l, ((uint32_t)screen_size.x + 7u) / 8u, ((uint32_t)screen_size.y + 7u) / 8u, 1);
	rd->compute_list_end();
}
void RadianceCascade::dispatch_dynamic_voxelize() {}
void RadianceCascade::dispatch_dyn_occ_temporal() {}

/* VOXEL SCENE / SDF */

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

void RadianceCascade::build_sdf_region(const Vector3i &p_hlo, const Vector3i &p_hdim) {
	// Localized jump flood: same seed → flood (R/2..1) → finalize as build_sdf, but restricted to
	// a window-relative half-res slab = the scrolled-in shell + a margin. Only that band is
	// re-dispatched; the rest of sdf_tex keeps its prior (valid, phase-independent) distances, and
	// the finalize mins the slab against them so a cell never gets a LARGER distance than before
	// (leak-safe). Cheap because the dispatch volume shrinks to the thin band -- the flood still
	// runs R/2..1 steps so lateral occluders inside the slab propagate fully (out-of-slab
	// neighbours are skipped in-shader; occluders beyond the margin are covered by the min).
	RadianceCascadeShaders &sh = *gi->rc_shader;
	const int R = vox_res / 2; // HALF res
	const int margin = 8; // half-res voxels of overlap into the existing field on every side
	Vector3i lo, dim;
	for (int i = 0; i < 3; i++) {
		const int a = MAX(0, p_hlo[i] - margin);
		const int b = MIN(R, p_hlo[i] + p_hdim[i] + margin);
		lo[i] = a;
		dim[i] = b - a;
	}
	if (dim.x <= 0 || dim.y <= 0 || dim.z <= 0) {
		return;
	}
	const uint32_t gx = (uint32_t)((dim.x + 3) / 4);
	const uint32_t gy = (uint32_t)((dim.y + 3) / 4);
	const uint32_t gz = (uint32_t)((dim.z + 3) / 4);
	auto run = [&](RID p_set, uint32_t p_mode, int p_step) {
		RCSdfPushConstant pc = {};
		pc.mode = p_mode;
		pc.step = p_step;
		pc.res = (uint32_t)R;
		pc.region = 1u;
		pc.phase[0] = vox_phase.x / 2;
		pc.phase[1] = vox_phase.y / 2;
		pc.phase[2] = vox_phase.z / 2;
		pc.slab_lo[0] = lo.x;
		pc.slab_lo[1] = lo.y;
		pc.slab_lo[2] = lo.z;
		pc.slab_dim[0] = dim.x;
		pc.slab_dim[1] = dim.y;
		pc.slab_dim[2] = dim.z;
		RD::ComputeListID l = rd->compute_list_begin();
		rd->compute_list_bind_compute_pipeline(l, sh.voxel_sdf_pipeline);
		rd->compute_list_bind_uniform_set(l, p_set, 0);
		rd->compute_list_set_push_constant(l, &pc, sizeof(pc));
		rd->compute_list_dispatch(l, gx, gy, gz);
		rd->compute_list_end();
	};
	run(sdf_set_write_a, 0u, 0); // seed the slab from occupancy
	bool seed_in_a = true;
	for (int step = R / 2; step >= 1; step >>= 1) {
		run(seed_in_a ? sdf_set_write_b : sdf_set_write_a, 1u, step);
		seed_in_a = !seed_in_a;
	}
	run(seed_in_a ? sdf_set_write_b : sdf_set_write_a, 2u, 0); // finalize (min with prior field)
}

void RadianceCascade::sdf_amortize_begin() {
	sdf_pass = 0;
	sdf_seed_in_a = true;
}

bool RadianceCascade::sdf_amortize_step() {
	// Spread the jump flood (1 seed + ~log2(R) flood + 1 finalize passes) across frames at
	// ~2 passes/frame instead of all at once -- this is the single biggest rebake spike, and
	// the one SDFGI pays in full on every motion. The SDF is only used for empty-space skip,
	// so a few frames of a converging field cost a little trace speed / brief leak, never a
	// frame drop. Returns true on the frame it finishes (so the caller can re-light correctly).
	if (sdf_pass < 0) {
		return false;
	}
	RadianceCascadeShaders &sh = *gi->rc_shader;
	const int R = vox_res / 2;
	int flood = 0;
	for (int s = R / 2; s >= 1; s >>= 1) {
		++flood;
	}
	const int total = 1 + flood + 1;
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
	for (int n = 0; n < 2 && sdf_pass < total; ++n, ++sdf_pass) {
		if (sdf_pass == 0) {
			run(sdf_set_write_a, 0u, 0); // seed from occupancy
			sdf_seed_in_a = true;
		} else if (sdf_pass <= flood) {
			const int step = 1 << (flood - sdf_pass); // R/2 .. 1
			run(sdf_seed_in_a ? sdf_set_write_b : sdf_set_write_a, 1u, step);
			sdf_seed_in_a = !sdf_seed_in_a;
		} else {
			run(sdf_seed_in_a ? sdf_set_write_b : sdf_set_write_a, 2u, 0); // finalize to distances
		}
	}
	if (sdf_pass >= total) {
		sdf_pass = -1;
		return true; // flood complete this frame
	}
	return false;
}
