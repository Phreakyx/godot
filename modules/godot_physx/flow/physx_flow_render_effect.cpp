/**************************************************************************/
/*  physx_flow_render_effect.cpp                                          */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
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

#include "physx_flow_render_effect.h"

#include "../nodes/physx_compositor_effects.h"
#include "flow_backend.h"
#include "physx_flow_3d.h"

#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "scene/3d/camera_3d.h"
#include "scene/resources/3d/world_3d.h"
#include "servers/rendering/renderer_rd/storage_rd/render_data_rd.h"
#include "servers/rendering/renderer_rd/storage_rd/render_scene_buffers_rd.h"
#include "servers/rendering/renderer_rd/storage_rd/render_scene_data_rd.h"
#include "servers/rendering/rendering_device.h"
#include "servers/rendering/rendering_device_binds.h"
#include "servers/rendering/rendering_server.h"
#include "servers/rendering/storage/render_data.h"

LocalVector<PhysXFlow3D *> PhysXFlowRenderEffect::flows;
HashMap<ObjectID, int> PhysXFlowRenderEffect::worlds;

namespace {

Ref<PhysXFlowRenderEffect> shared_effect;

const char *COPY_SHADER = R"(
#[compute]
#version 450

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0, rgba16f) uniform restrict readonly image2D src;
layout(set = 0, binding = 1, rgba16f) uniform restrict writeonly image2D dst;

layout(push_constant, std430) uniform Params {
	ivec2 size;
	ivec2 pad;
} params;

void main() {
	ivec2 p = ivec2(gl_GlobalInvocationID.xy);
	if (p.x >= params.size.x || p.y >= params.size.y) {
		return;
	}
	imageStore(dst, p, imageLoad(src, p));
}
)";

// Godot's column-vector matrix as Flow's row-vector one: Flow's row i is
// Godot's column i, which is exactly Projection's memory layout.
void to_flow(const Projection &p_m, float r_out[16]) {
	for (int c = 0; c < 4; c++) {
		for (int r = 0; r < 4; r++) {
			r_out[c * 4 + r] = (float)p_m.columns[c][r];
		}
	}
}

} // namespace

void PhysXFlowRenderEffect::register_flow(PhysXFlow3D *p_flow) {
	if (flows.find(p_flow) < 0) {
		flows.push_back(p_flow);
	}
}

void PhysXFlowRenderEffect::unregister_flow(PhysXFlow3D *p_flow) {
	flows.erase(p_flow);
}

namespace {

} // namespace

void PhysXFlowRenderEffect::attach_world(World3D *p_world) {
	ERR_FAIL_NULL(p_world);
	worlds[p_world->get_instance_id()]++;
}

void PhysXFlowRenderEffect::detach_world(const ObjectID &p_world) {
	int *count = worlds.getptr(p_world);
	if (count == nullptr || --(*count) > 0) {
		return;
	}
	worlds.erase(p_world);
	PhysXCompositorEffects::uninstall(p_world, get_shared());
}

void PhysXFlowRenderEffect::install(World3D *p_world, Camera3D *p_camera) {
	ERR_FAIL_NULL(p_world);
	ERR_FAIL_COND(!worlds.has(p_world->get_instance_id()));
	PhysXCompositorEffects::install(p_world, p_camera, get_shared(), PhysXCompositorEffects::ORDER_FLOW);
}

Ref<Compositor> PhysXFlowRenderEffect::get_world_compositor(const Ref<World3D> &p_world) {
	return p_world.is_valid() ? p_world->get_compositor() : Ref<Compositor>();
}

void PhysXFlowRenderEffect::_bind_methods() {
	ClassDB::bind_static_method("PhysXFlowRenderEffect", D_METHOD("get_world_compositor", "world"), &PhysXFlowRenderEffect::get_world_compositor);
}

Ref<PhysXFlowRenderEffect> PhysXFlowRenderEffect::get_shared() {
	if (shared_effect.is_null()) {
		shared_effect.instantiate();
	}
	return shared_effect;
}

void PhysXFlowRenderEffect::free_shared() {
	shared_effect.unref();
}

PhysXFlowRenderEffect::PhysXFlowRenderEffect() {
	set_effect_callback_type(EFFECT_CALLBACK_TYPE_POST_TRANSPARENT);
	set_access_resolved_color(true);
	set_access_resolved_depth(true);
	// Straight to C++, instead of the script-facing _render_callback.
	RenderingServer::get_singleton()->compositor_effect_set_callback(get_rid(), RSE::COMPOSITOR_EFFECT_CALLBACK_TYPE_POST_TRANSPARENT, callable_mp(this, &PhysXFlowRenderEffect::_render));
}

PhysXFlowRenderEffect::~PhysXFlowRenderEffect() {
	RenderingDevice *rd = RenderingServer::get_singleton() ? RenderingServer::get_singleton()->get_rendering_device() : nullptr;
	if (rd != nullptr) {
		if (copy_pipeline.is_valid()) {
			rd->free_rid(copy_pipeline);
		}
		if (copy_shader.is_valid()) {
			rd->free_rid(copy_shader);
		}
		scatter.free(rd);
	}
}

bool PhysXFlowRenderEffect::_ensure_copy_pipeline(RenderingDevice *p_rd) {
	if (copy_pipeline.is_valid()) {
		return true;
	}
	if (copy_failed) {
		return false;
	}
	Ref<RDShaderFile> file;
	file.instantiate();
	if (file->parse_versions_from_text(COPY_SHADER) != OK) {
		copy_failed = true;
		ERR_FAIL_V_MSG(false, "PhysXFlowRenderEffect: copy shader failed to compile.");
	}
	copy_shader = p_rd->shader_create_from_spirv(file->get_spirv_stages());
	if (copy_shader.is_valid()) {
		copy_pipeline = p_rd->compute_pipeline_create(copy_shader);
	}
	copy_failed = !copy_pipeline.is_valid();
	return !copy_failed;
}

void PhysXFlowRenderEffect::copy_texture(RenderingDevice *p_rd, const RID &p_src, const RID &p_dst, const Size2i &p_size) {
	if (!_ensure_copy_pipeline(p_rd)) {
		return;
	}
	RD::Uniform src(RD::UNIFORM_TYPE_IMAGE, 0, p_src);
	RD::Uniform dst(RD::UNIFORM_TYPE_IMAGE, 1, p_dst);
	LocalVector<RD::Uniform> uniforms;
	uniforms.push_back(src);
	uniforms.push_back(dst);
	const RID set = p_rd->uniform_set_create(uniforms, copy_shader, 0);
	ERR_FAIL_COND(!set.is_valid());
	const int32_t push[4] = { p_size.x, p_size.y, 0, 0 };
	const RD::ComputeListID list = p_rd->compute_list_begin();
	p_rd->compute_list_bind_compute_pipeline(list, copy_pipeline);
	p_rd->compute_list_bind_uniform_set(list, set, 0);
	p_rd->compute_list_set_push_constant(list, push, sizeof(push));
	p_rd->compute_list_dispatch(list, (p_size.x + 7) / 8, (p_size.y + 7) / 8, 1);
	p_rd->compute_list_end();
	p_rd->free_rid(set);
}

void PhysXFlowRenderEffect::_render(int p_callback_type, const RenderData *p_render_data) {
	if (flows.is_empty() || p_render_data == nullptr) {
		return;
	}
	RenderSceneBuffersRD *buffers = Object::cast_to<RenderSceneBuffersRD>(p_render_data->get_render_scene_buffers().ptr());
	RenderSceneDataRD *scene = Object::cast_to<RenderSceneDataRD>(p_render_data->get_render_scene_data());
	RenderingDevice *rd = RenderingServer::get_singleton()->get_rendering_device();
	if (buffers == nullptr || scene == nullptr || rd == nullptr) {
		return;
	}
	const Size2i size = buffers->get_internal_size();
	const RID color = buffers->get_internal_texture();
	// (A screen-space fluid has written its surface into this depth: the gas
	// is marched up to the liquid.)
	const RID depth = buffers->get_depth_texture();
	if (size.x <= 0 || size.y <= 0 || !color.is_valid() || !depth.is_valid()) {
		return;
	}

	// The projection the depth buffer was rendered with: Godot's camera
	// projection already carries its depth correction (reverse Z in 0..1, Y
	// as this target needs) and TAA jitter, so Flow reconstructs scene
	// positions from depth exactly.
	Projection projection = scene->get_cam_projection();
	// Flow reads its targets with row 0 at clip-space +Y; Godot's are the
	// other way up (checked on screen: without this the plume is upside down).
	Projection y_flip;
	y_flip.columns[1][1] = -1.0;
	projection = y_flip * projection;
	// Flow's world is in its own units: scale down to meters first.
	Projection to_meters;
	const real_t inv = 1.0 / FlowBackend::UNITS_PER_METER;
	to_meters.columns[0][0] = inv;
	to_meters.columns[1][1] = inv;
	to_meters.columns[2][2] = inv;
	const Projection view = Projection(scene->get_cam_transform().affine_inverse()) * to_meters;
	float view_f[16];
	float projection_f[16];
	to_flow(view, view_f);
	to_flow(projection, projection_f);
	// For the light scattering pass: clip space (as the depth buffer has it)
	// straight back to world meters.
	const Projection inv_view_projection = (scene->get_cam_projection() * Projection(scene->get_cam_transform().affine_inverse())).inverse();
	// The frame's lights and shadow atlas, for the scattering pass's shadows.
	const RenderDataRD *render_data_rd = dynamic_cast<const RenderDataRD *>(p_render_data);

	// Each flow composites over what's drawn so far, so far ones go first.
	const Vector3 eye = scene->get_cam_transform().origin;
	LocalVector<PhysXFlow3D *> sorted;
	LocalVector<real_t> distance;
	for (PhysXFlow3D *flow : flows) {
		sorted.push_back(flow);
		distance.push_back(eye.distance_squared_to(flow->get_draw_center()));
	}
	for (uint32_t i = 1; i < sorted.size(); i++) {
		for (uint32_t j = i; j > 0 && distance[j - 1] < distance[j]; j--) {
			SWAP(distance[j - 1], distance[j]);
			SWAP(sorted[j - 1], sorted[j]);
		}
	}
	for (PhysXFlow3D *flow : sorted) {
		flow->render_rd(this, rd, view_f, projection_f, size, depth, color, inv_view_projection, eye, render_data_rd != nullptr ? render_data_rd->lights : nullptr, render_data_rd != nullptr ? render_data_rd->shadow_atlas : RID());
	}
}
