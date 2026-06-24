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
	screen_size = p_size;
	half_size = Size2i(p_size.x / 2, p_size.y / 2);
	// TODO(port): allocate textures/buffers/uniform sets; build the cascade table.
}

void RadianceCascade::process(RenderDataRD *p_render_data) {
	// TODO(port): the full per-frame chain (see header), writing RB_TEX_AMBIENT.
}

/* CASCADE TABLE */

void RadianceCascade::build_cascade_table() {
	// TODO(port): fill cascades[] + offsets/sizes from the tuning knobs; upload.
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
