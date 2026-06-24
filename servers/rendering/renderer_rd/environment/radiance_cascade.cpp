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
	// TODO(port): version_create() + compute_pipeline_create() for every pass,
	// with the TraceBackend variants on patch_trace.
}

void RadianceCascadeShaders::free() {
	// TODO(port): version_free() every shader.
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
