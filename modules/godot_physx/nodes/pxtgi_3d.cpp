/**************************************************************************/
/*  pxtgi_3d.cpp                                                          */
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

#include "pxtgi_3d.h"

#include "../godot_physx_server_3d.h"
#include "../objects/godot_physx_body_3d.h"
#include "../spaces/godot_physx_space_3d.h"
#include "physx_gi_effect.h"

#include "core/config/engine.h"
#include "core/math/convex_hull.h"
#include "core/object/callable_mp.h"
#include "core/templates/hashfuncs.h"
#include "scene/3d/camera_3d.h"
#include "scene/3d/light_3d.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/3d/physics/collision_object_3d.h"
#include "scene/3d/physics/collision_shape_3d.h"
#include "scene/main/scene_tree.h"
#include "scene/main/viewport.h"
#include "scene/main/window.h"
#include "scene/resources/3d/box_shape_3d.h"
#include "scene/resources/3d/capsule_shape_3d.h"
#include "scene/resources/3d/concave_polygon_shape_3d.h"
#include "scene/resources/3d/convex_polygon_shape_3d.h"
#include "scene/resources/3d/cylinder_shape_3d.h"
#include "scene/resources/3d/height_map_shape_3d.h"
#include "scene/resources/3d/sphere_shape_3d.h"
#include "scene/resources/3d/world_3d.h"
#include "scene/resources/material.h"

#ifdef TOOLS_ENABLED
#include "main/main.h"
#endif
#include "servers/rendering/renderer_rd/environment/gi.h"
#include "servers/rendering/renderer_rd/renderer_scene_render_rd.h"
#include "servers/rendering/rendering_device_binds.h"
#include "servers/rendering/rendering_server.h"

#include <algorithm>

int PXTGI3D::users = 0;

namespace {

RID hook_shader;

// The binding layout of Godot's gi.glsl, declared exactly (every binding used, so none is stripped), so the uniform
// sets process_gi builds for its own shader bind to this one too. @VRS@ is replaced with the VRS image binding when the
// GPU supports VRS: Godot then builds its uniform set against the VRS variant.
const char *HOOK_SHADER = R"(
#[compute]
#version 450

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(constant_id = 0) const bool sc_half_res = false;
layout(constant_id = 1) const bool sc_use_full_projection_matrix = false;
layout(constant_id = 2) const bool sc_use_vrs = false;

layout(set = 0, binding = 1) uniform texture3D sdf_cascades[8];
layout(set = 0, binding = 2) uniform texture3D light_cascades[8];
layout(set = 0, binding = 3) uniform texture3D aniso0_cascades[8];
layout(set = 0, binding = 4) uniform texture3D aniso1_cascades[8];
layout(set = 0, binding = 5) uniform texture3D occlusion_texture;
layout(set = 0, binding = 6) uniform sampler linear_sampler;
layout(set = 0, binding = 7) uniform sampler linear_sampler_with_mipmaps;
layout(rgba16f, set = 0, binding = 9) uniform restrict writeonly image2D ambient_buffer;
layout(rgba16f, set = 0, binding = 10) uniform restrict writeonly image2D reflection_buffer;
layout(set = 0, binding = 11) uniform texture2DArray lightprobe_texture;
layout(set = 0, binding = 12) uniform texture2D depth_buffer;
layout(set = 0, binding = 13) uniform texture2D normal_roughness_buffer;
layout(set = 0, binding = 14) uniform utexture2D voxel_gi_buffer;
struct ProbeCascadeData {
	vec3 position;
	float to_probe;
	ivec3 probe_world_offset;
	float to_cell;
	vec3 pad;
	float exposure_normalization;
};
layout(set = 0, binding = 15, std140) uniform SDFGI {
	vec3 grid_size;
	uint max_cascades;
	bool use_occlusion;
	int probe_axis_size;
	float probe_to_uvw;
	float normal_bias;
	vec3 lightprobe_tex_pixel_size;
	float energy;
	vec3 lightprobe_uv_offset;
	float y_mult;
	vec3 occlusion_clamp;
	uint pad3;
	vec3 occlusion_renormalize;
	uint pad4;
	vec3 cascade_probe_size;
	uint pad5;
	ProbeCascadeData cascades[8];
}
sdfgi;
struct VoxelGIData {
	mat4 xform;
	vec3 bounds;
	float dynamic_range;
	float bias;
	float normal_bias;
	bool blend_ambient;
	uint mipmaps;
	vec3 pad;
	float exposure_normalization;
};
layout(set = 0, binding = 16, std140) uniform VoxelGIs {
	VoxelGIData data[8];
}
voxel_gi_instances;
layout(set = 0, binding = 17) uniform texture3D voxel_gi_textures[8];
layout(set = 0, binding = 18, std140) uniform SceneData {
	mat4x4 inv_projection[2];
	mat4x4 cam_transform;
	vec4 eye_offset[2];
	ivec2 screen_size;
	float pad1;
	float pad2;
}
scene_data;
@VRS@

layout(push_constant, std430) uniform Params {
	uint max_voxel_gi_instances;
	bool high_quality_vct;
	bool orthogonal;
	uint view_index;
	vec4 proj_info;
	float z_near;
	float z_far;
	float pad2;
	float pad3;
}
params;

vec3 view_position(ivec2 screen_pos) {
	float depth = texelFetch(sampler2D(depth_buffer, linear_sampler), screen_pos, 0).r;
	if (sc_use_full_projection_matrix) {
		vec4 pos = vec4((2.0 * vec2(screen_pos) / vec2(scene_data.screen_size)) - 1.0, depth, 1.0);
		pos = scene_data.inv_projection[params.view_index] * pos;
		return pos.xyz / pos.w;
	}
	vec3 pos;
	pos.z = depth * 2.0 - 1.0;
	if (params.orthogonal) {
		pos.z = -(pos.z * (params.z_far - params.z_near) - (params.z_far + params.z_near)) / 2.0;
	} else {
		pos.z = 2.0 * params.z_near * params.z_far / (params.z_far + params.z_near + pos.z * (params.z_far - params.z_near));
	}
	pos.z = -pos.z;
	pos.xy = vec2(screen_pos) * params.proj_info.xy + params.proj_info.zw;
	if (!params.orthogonal) {
		pos.xy *= pos.z;
	}
	pos.y = -pos.y;
	return pos;
}

// Never true (Godot's view index is 0 or 1): keeps every binding in the compiled shader so the layout matches.
void keep_bindings(ivec2 pos) {
	if (params.view_index == 0xFFFFFFFFu) {
		vec4 k = texture(sampler3D(sdf_cascades[0], linear_sampler), vec3(0.0)) + texture(sampler3D(light_cascades[0], linear_sampler), vec3(0.0)) +
				texture(sampler3D(aniso0_cascades[0], linear_sampler), vec3(0.0)) + texture(sampler3D(aniso1_cascades[0], linear_sampler), vec3(0.0)) +
				texture(sampler3D(occlusion_texture, linear_sampler_with_mipmaps), vec3(0.0)) + texture(sampler2DArray(lightprobe_texture, linear_sampler), vec3(0.0)) +
				texture(sampler2D(normal_roughness_buffer, linear_sampler), vec2(0.0)) + vec4(texelFetch(usampler2D(voxel_gi_buffer, linear_sampler), ivec2(0), 0)) +
				vec4(sdfgi.grid_size, sdfgi.energy) + voxel_gi_instances.data[0].xform[0] + texture(sampler3D(voxel_gi_textures[0], linear_sampler), vec3(0.0)) + scene_data.eye_offset[0] @VRS_USE@;
		imageStore(reflection_buffer, pos, k);
	}
}

void main() {
	ivec2 pos = ivec2(gl_GlobalInvocationID.xy);
	if (sc_half_res) {
		pos <<= 1;
	}
	if (any(greaterThanEqual(pos, scene_data.screen_size))) {
		return;
	}
	keep_bindings(pos);
	// Nothing else: the module's PhysXGIEffect already wrote this frame's GI into these buffers, before the opaque
	// pass; this step only has to leave them alone.
}
)";

Vector<RD::PipelineSpecializationConstant> specialization(int p_variation) {
	Vector<RD::PipelineSpecializationConstant> constants;
	for (int i = 0; i < 3; i++) {
		RD::PipelineSpecializationConstant sc;
		sc.type = RD::PIPELINE_SPECIALIZATION_CONSTANT_TYPE_BOOL;
		sc.constant_id = i;
		sc.bool_value = (p_variation & (1 << i)) != 0;
		constants.push_back(sc);
	}
	return constants;
}

} // namespace

// On the render thread.
void PXTGI3D::_install() {
	RendererSceneRenderRD *scene_render = RendererSceneRenderRD::get_singleton();
	RenderingDevice *rd = RenderingDevice::get_singleton();
	ERR_FAIL_COND(scene_render == nullptr || rd == nullptr);
	RendererRD::GI *gi = scene_render->get_gi();
	if (hook_shader.is_null()) {
		const bool vrs = scene_render->is_vrs_supported();
		String source = String(HOOK_SHADER)
								.replace("@VRS@", vrs ? "layout(r8ui, set = 0, binding = 19) uniform restrict readonly uimage2D vrs_buffer;" : "")
								.replace("@VRS_USE@", vrs ? "+ vec4(imageLoad(vrs_buffer, ivec2(0)))" : "");
		Ref<RDShaderFile> file;
		file.instantiate();
		ERR_FAIL_COND_MSG(file->parse_versions_from_text(source) != OK, "PXTGI3D: the hook shader failed to parse.");
		Ref<RDShaderSPIRV> spirv = file->get_spirv();
		ERR_FAIL_COND(spirv.is_null());
		const String err = spirv->get_stage_compile_error(RD::SHADER_STAGE_COMPUTE);
		ERR_FAIL_COND_MSG(!err.is_empty(), "PXTGI3D: " + err);
		hook_shader = rd->shader_create_from_spirv(file->get_spirv_stages());
		ERR_FAIL_COND(hook_shader.is_null());
	}
	for (int v = 0; v < RendererRD::GI::SHADER_SPECIALIZATION_VARIATIONS; v++) {
		for (int mode : { RendererRD::GI::MODE_SDFGI, RendererRD::GI::MODE_COMBINED, RendererRD::GI::MODE_COMBINED_WITHOUT_SAMPLER }) {
			gi->pipelines[v][mode].create_compute_pipeline(hook_shader, specialization(v));
		}
	}
	print_verbose("PXTGI3D: GI step hooked.");
}

// On the render thread: Godot's own pipelines back, exactly as GI::init builds them.
void PXTGI3D::_uninstall() {
	RendererSceneRenderRD *scene_render = RendererSceneRenderRD::get_singleton();
	if (scene_render == nullptr) {
		return;
	}
	RendererRD::GI *gi = scene_render->get_gi();
	const int variant_base = scene_render->is_vrs_supported() ? RendererRD::GI::MODE_MAX : 0;
	for (int v = 0; v < RendererRD::GI::SHADER_SPECIALIZATION_VARIATIONS; v++) {
		for (int mode : { RendererRD::GI::MODE_SDFGI, RendererRD::GI::MODE_COMBINED, RendererRD::GI::MODE_COMBINED_WITHOUT_SAMPLER }) {
			gi->pipelines[v][mode].create_compute_pipeline(gi->shader.version_get_shader(gi->shader_version, variant_base + mode), specialization(v));
		}
	}
	print_verbose("PXTGI3D: GI step restored.");
}

// At shutdown: the do-nothing shader (Godot's own GI step is back by then).
void PXTGI3D::free_shared() {
	RenderingDevice *rd = RenderingDevice::get_singleton();
	if (hook_shader.is_valid() && rd) {
		rd->free_rid(hook_shader);
	}
	hook_shader = RID();
}

void PXTGI3D::_attach() {
	if (attached_world.is_valid() || !is_inside_world()) {
		return;
	}
	World3D *world = get_world_3d().ptr();
	if (world == nullptr) {
		return;
	}
	PhysXGIEffect::attach_world(world);
	attached_world = world->get_instance_id();
}

// In the world and visible: deliver the GI (the first such node hooks Godot's GI step).
void PXTGI3D::_activate() {
	if (active) {
		return;
	}
	active = true;
	_attach();
	if (users++ == 0) {
		RenderingServer::get_singleton()->environment_set_sdfgi_ray_count(RSE::ENV_SDFGI_RAY_COUNT_4);
		RenderingServer::get_singleton()->environment_set_sdfgi_frames_to_update_light(RSE::ENV_SDFGI_UPDATE_LIGHT_IN_16_FRAMES);
		RenderingServer::get_singleton()->call_on_render_thread(callable_mp_static(&PXTGI3D::_install));
	}
	set_process_internal(true);
}

// Out of the world or hidden: Godot's own GI back (the last such node restores its GI step).
void PXTGI3D::_deactivate() {
	if (!active) {
		return;
	}
	active = false;
	set_process_internal(false);
	_detach();
	if (--users == 0) {
		RenderingServer::get_singleton()->call_on_render_thread(callable_mp_static(&PXTGI3D::_uninstall));
	}
}

void PXTGI3D::_detach() {
	PhysXGIEffect::remove_settings(get_instance_id());
	// The environment's own SDFGI settings back (setting a property re-sends them all).
	if (environment.is_valid()) {
		environment->set_sdfgi_enabled(environment->is_sdfgi_enabled());
		environment.unref();
	}
	if (attached_world.is_valid()) {
		PhysXGIEffect::detach_world(attached_world);
		attached_world = ObjectID();
	}
}

namespace {

// A texture's average colour (linear), once per texture: a masked emission's overall glow.
Color texture_average(const Ref<Texture2D> &p_texture) {
	static HashMap<ObjectID, Color> known;
	if (const Color *c = known.getptr(p_texture->get_instance_id())) {
		return *c;
	}
	Color average(1, 1, 1);
	Ref<Image> image = p_texture->get_image();
	if (image.is_valid() && !image->is_empty()) {
		image = image->duplicate();
		if (image->is_compressed()) {
			image->decompress();
		}
		const int w = image->get_width();
		const int h = image->get_height();
		Color sum(0, 0, 0, 0);
		int n = 0;
		for (int y = 0; y < 32; y++) {
			for (int x = 0; x < 32; x++) {
				sum += image->get_pixel((x * 2 + 1) * w / 64, (y * 2 + 1) * h / 64).srgb_to_linear();
				n++;
			}
		}
		average = sum / float(n);
	}
	known.insert(p_texture->get_instance_id(), average);
	return average;
}

// A mesh's albedo and emitted light (linear; emission times its energy, in the units Godot adds it to the frame; an
// emission texture by its average).
void mesh_look(MeshInstance3D *p_mesh, Color &r_albedo, Color &r_emission) {
	r_albedo = Color(0.5, 0.5, 0.5);
	r_emission = Color(0, 0, 0);
	Ref<BaseMaterial3D> material = p_mesh->get_active_material(0);
	if (material.is_null()) {
		return;
	}
	r_albedo = material->get_albedo().srgb_to_linear();
	if (material->get_feature(BaseMaterial3D::FEATURE_EMISSION)) {
		const Color base = material->get_emission().srgb_to_linear();
		Ref<Texture2D> texture = material->get_texture(BaseMaterial3D::TEXTURE_EMISSION);
		Color glow = base;
		if (texture.is_valid()) {
			const Color average = texture_average(texture);
			glow = material->get_emission_operator() == BaseMaterial3D::EMISSION_OP_ADD ? base + average : base * average;
		}
		glow.a = 1.0f;
		r_emission = glow * material->get_emission_energy_multiplier();
	}
}

// The look of the first mesh under a body, or a neutral grey.
void body_look(Node *p_body, Color &r_albedo, Color &r_emission) {
	r_albedo = Color(0.5, 0.5, 0.5);
	r_emission = Color(0, 0, 0);
	for (int i = 0; i < p_body->get_child_count(); i++) {
		if (MeshInstance3D *mesh = Object::cast_to<MeshInstance3D>(p_body->get_child(i))) {
			mesh_look(mesh, r_albedo, r_emission);
			return;
		}
	}
}

bool is_emissive_mesh(Node *p_node) {
	MeshInstance3D *mesh = Object::cast_to<MeshInstance3D>(p_node);
	if (mesh == nullptr || Object::cast_to<CollisionObject3D>(p_node->get_parent()) || mesh->get_mesh().is_null()) {
		return false;
	}
	Color albedo, emission;
	mesh_look(mesh, albedo, emission);
	return emission.get_luminance() > 0.0f;
}

void collect(Node *p_node, LocalVector<Node *> &r_shapes, ObjectID &r_sun, LocalVector<ObjectID> &r_omnis, LocalVector<ObjectID> &r_spots) {
	if ((Object::cast_to<CollisionShape3D>(p_node) && Object::cast_to<CollisionObject3D>(p_node->get_parent())) || is_emissive_mesh(p_node)) {
		r_shapes.push_back(p_node);
	}
	if (r_sun.is_null() && Object::cast_to<DirectionalLight3D>(p_node)) {
		r_sun = p_node->get_instance_id();
	}
	if (Object::cast_to<OmniLight3D>(p_node) && r_omnis.size() < (uint32_t)PhysXGIEffect::Snapshot::MAX_OMNIS) {
		r_omnis.push_back(p_node->get_instance_id());
	}
	if (Object::cast_to<SpotLight3D>(p_node) && r_spots.size() < (uint32_t)PhysXGIEffect::Snapshot::MAX_SPOTS) {
		r_spots.push_back(p_node->get_instance_id());
	}
	for (int i = 0; i < p_node->get_child_count(); i++) {
		collect(p_node->get_child(i), r_shapes, r_sun, r_omnis, r_spots);
	}
}

struct BuildItem {
	AABB bounds;
	Vector3 center;
	int index = 0;
};

// Median split along the longest axis of the centres, leaves of up to four; appends nodes depth first (an inner node's
// left child right after it) and the primitives in leaf order.
int build_bvh(LocalVector<BuildItem> &p_items, uint32_t p_from, uint32_t p_to, const float *p_source, LocalVector<float> &r_nodes, LocalVector<float> &r_primitives, LocalVector<int> &r_order) {
	AABB bounds = p_items[p_from].bounds;
	AABB centres(p_items[p_from].center, Vector3());
	for (uint32_t i = p_from + 1; i < p_to; i++) {
		bounds.merge_with(p_items[i].bounds);
		centres.expand_to(p_items[i].center);
	}
	const int node = r_nodes.size() / PhysXGIEffect::NODE_FLOATS;
	r_nodes.resize(r_nodes.size() + PhysXGIEffect::NODE_FLOATS);
	float *out = &r_nodes[node * PhysXGIEffect::NODE_FLOATS];
	const Vector3 lo = bounds.position;
	const Vector3 hi = bounds.position + bounds.size;
	out[0] = lo.x;
	out[1] = lo.y;
	out[2] = lo.z;
	out[4] = hi.x;
	out[5] = hi.y;
	out[6] = hi.z;
	const uint32_t count = p_to - p_from;
	if (count <= 4) {
		const int first = r_primitives.size() / PhysXGIEffect::PRIMITIVE_FLOATS;
		for (uint32_t i = p_from; i < p_to; i++) {
			const float *src = p_source + p_items[i].index * PhysXGIEffect::PRIMITIVE_FLOATS;
			r_order.push_back(p_items[i].index);
			for (int k = 0; k < PhysXGIEffect::PRIMITIVE_FLOATS; k++) {
				r_primitives.push_back(src[k]);
			}
		}
		memcpy(&out[3], &first, sizeof(int));
		const int leaf_count = count;
		memcpy(&out[7], &leaf_count, sizeof(int));
		return node;
	}
	const int axis = centres.get_longest_axis_index();
	const uint32_t mid = p_from + count / 2;
	std::nth_element(p_items.ptr() + p_from, p_items.ptr() + mid, p_items.ptr() + p_to, [axis](const BuildItem &a, const BuildItem &b) {
		return a.center[axis] < b.center[axis];
	});
	build_bvh(p_items, p_from, mid, p_source, r_nodes, r_primitives, r_order);
	const int right = build_bvh(p_items, mid, p_to, p_source, r_nodes, r_primitives, r_order);
	out = &r_nodes[node * PhysXGIEffect::NODE_FLOATS];
	memcpy(&out[3], &right, sizeof(int));
	const int zero = 0;
	memcpy(&out[7], &zero, sizeof(int));
	return node;
}

struct TriangleItem {
	AABB bounds;
	Vector3 center;
	int index = 0;
};

void store_int(float *r_at, int p_value) {
	memcpy(r_at, &p_value, sizeof(int));
}

// A mesh's tree over its triangles (median split, leaves of up to four), in the scene tree's node layout; the
// triangles reordered to its leaves.
int build_triangle_tree(LocalVector<TriangleItem> &p_items, uint32_t p_from, uint32_t p_to, const Vector<Vector3> &p_corners, Vector<float> &r_nodes, Vector<float> &r_triangles) {
	AABB bounds = p_items[p_from].bounds;
	AABB centres(p_items[p_from].center, Vector3());
	for (uint32_t i = p_from + 1; i < p_to; i++) {
		bounds.merge_with(p_items[i].bounds);
		centres.expand_to(p_items[i].center);
	}
	const int node = r_nodes.size() / 8;
	r_nodes.resize(r_nodes.size() + 8);
	float *out = r_nodes.ptrw() + node * 8;
	const Vector3 lo = bounds.position;
	const Vector3 hi = bounds.position + bounds.size;
	out[0] = lo.x;
	out[1] = lo.y;
	out[2] = lo.z;
	out[4] = hi.x;
	out[5] = hi.y;
	out[6] = hi.z;
	const uint32_t count = p_to - p_from;
	if (count <= 4) {
		store_int(&out[3], r_triangles.size() / 12);
		store_int(&out[7], count);
		for (uint32_t i = p_from; i < p_to; i++) {
			for (int c = 0; c < 3; c++) {
				const Vector3 v = p_corners[p_items[i].index * 3 + c];
				r_triangles.push_back(v.x);
				r_triangles.push_back(v.y);
				r_triangles.push_back(v.z);
				r_triangles.push_back(0.0f);
			}
		}
		return node;
	}
	const int axis = centres.get_longest_axis_index();
	const uint32_t mid = p_from + count / 2;
	std::nth_element(p_items.ptr() + p_from, p_items.ptr() + mid, p_items.ptr() + p_to, [axis](const TriangleItem &a, const TriangleItem &b) {
		return a.center[axis] < b.center[axis];
	});
	build_triangle_tree(p_items, p_from, mid, p_corners, r_nodes, r_triangles);
	const int right = build_triangle_tree(p_items, mid, p_to, p_corners, r_nodes, r_triangles);
	out = r_nodes.ptrw() + node * 8;
	store_int(&out[3], right);
	store_int(&out[7], 0);
	return node;
}

} // namespace

// The planes of a convex hull, or a triangle mesh's (or height map's) tree and triangles, in the shape's local space;
// built the first time the shape is seen.
const PXTGI3D::GeometryBlock *PXTGI3D::_geometry_for(const Ref<Shape3D> &p_shape) {
	const ObjectID id = p_shape->get_instance_id();
	if (const GeometryBlock *known = geometry_blocks.getptr(id)) {
		return known->type ? known : nullptr;
	}
	GeometryBlock block;
	Vector<Vector3> corners;
	if (Ref<ConvexPolygonShape3D> convex = p_shape; convex.is_valid()) {
		const Vector<Vector3> points = convex->get_points();
		Geometry3D::MeshData hull;
		if (points.size() >= 4 && ConvexHullComputer::convex_hull(points, hull) == OK && hull.faces.size() >= 4) {
			block.type = 4;
			for (const Geometry3D::MeshData::Face &face : hull.faces) {
				block.planes.push_back(face.plane.normal.x);
				block.planes.push_back(face.plane.normal.y);
				block.planes.push_back(face.plane.normal.z);
				block.planes.push_back(face.plane.d);
			}
			block.bounds = AABB(points[0], Vector3());
			for (const Vector3 &v : points) {
				block.bounds.expand_to(v);
			}
		}
	} else if (Ref<ConcavePolygonShape3D> concave = p_shape; concave.is_valid()) {
		corners = concave->get_faces();
	} else if (Ref<HeightMapShape3D> height = p_shape; height.is_valid()) {
		// Godot's height map: a grid one unit apart, centred on the shape's origin.
		const int w = height->get_map_width();
		const int d = height->get_map_depth();
		const Vector<real_t> data = height->get_map_data();
		if (w >= 2 && d >= 2 && data.size() >= w * d) {
			auto corner = [&](int x, int z) {
				return Vector3(x - (w - 1) * 0.5f, data[z * w + x], z - (d - 1) * 0.5f);
			};
			corners.resize((w - 1) * (d - 1) * 6);
			Vector3 *c = corners.ptrw();
			for (int z = 0; z < d - 1; z++) {
				for (int x = 0; x < w - 1; x++) {
					*c++ = corner(x, z);
					*c++ = corner(x + 1, z);
					*c++ = corner(x, z + 1);
					*c++ = corner(x + 1, z);
					*c++ = corner(x + 1, z + 1);
					*c++ = corner(x, z + 1);
				}
			}
		}
	}
	if (corners.size() >= 3) {
		LocalVector<TriangleItem> items;
		items.resize(corners.size() / 3);
		for (uint32_t i = 0; i < items.size(); i++) {
			AABB b(corners[i * 3], Vector3());
			b.expand_to(corners[i * 3 + 1]);
			b.expand_to(corners[i * 3 + 2]);
			items[i] = { b, b.get_center(), int(i) };
		}
		build_triangle_tree(items, 0, items.size(), corners, block.nodes, block.triangles);
		block.type = 5;
		block.bounds = AABB(corners[0], Vector3());
		for (const Vector3 &v : corners) {
			block.bounds.expand_to(v);
		}
	}
	geometry_blocks.insert(id, block);
	const GeometryBlock *stored = geometry_blocks.getptr(id);
	return stored->type ? stored : nullptr;
}

void PXTGI3D::_rescan() {
	shapes.clear();
	sun = ObjectID();
	omnis.clear();
	spots.clear();
	LocalVector<Node *> found;
	// In the editor, the scene being edited (not the editor's own nodes).
	Node *scan_root = get_tree()->get_root();
	if (Engine::get_singleton()->is_editor_hint() && get_tree()->get_edited_scene_root()) {
		scan_root = get_tree()->get_edited_scene_root();
	}
	collect(scan_root, found, sun, omnis, spots);
	for (Node *n : found) {
		ShapeEntry entry;
		entry.shape_node = n->get_instance_id();
		if (MeshInstance3D *mesh = Object::cast_to<MeshInstance3D>(n)) {
			entry.mesh_proxy = true;
			mesh_look(mesh, entry.albedo, entry.emission);
		} else {
			body_look(n->get_parent(), entry.albedo, entry.emission);
		}
		if (!entry.mesh_proxy) {
			CollisionShape3D *cs = Object::cast_to<CollisionShape3D>(n);
			if (cs->get_shape().is_valid()) {
				if (const GeometryBlock *block = _geometry_for(cs->get_shape())) {
					entry.geometry_type = block->type;
					entry.local_bounds = block->bounds;
				}
			}
		}
		shapes.push_back(entry);
	}
	body_entries.clear();
	proxy_entries.clear();
	emissive_entries.clear();
	for (uint32_t i = 0; i < shapes.size(); i++) {
		if (shapes[i].emission.get_luminance() > 0.0f) {
			emissive_entries.push_back(i);
		}
	}
	shape_signature = 0;
	for (uint32_t i = 0; i < shapes.size(); i++) {
		Object *node = ObjectDB::get_instance(shapes[i].shape_node);
		shape_signature = hash_murmur3_one_64(uint64_t(shapes[i].shape_node), shape_signature);
		if (shapes[i].mesh_proxy) {
			proxy_entries.push_back(i);
		} else if (CollisionShape3D *cs = Object::cast_to<CollisionShape3D>(node)) {
			body_entries[cs->get_parent()->get_instance_id()].push_back(i);
			shape_signature = hash_murmur3_one_64(cs->get_shape().is_valid() ? uint64_t(cs->get_shape()->get_instance_id()) : 0, shape_signature);
		}
	}
	// The geometry buffer: each hull's planes, each mesh's tree and triangles, laid end to end (in vec4s); rebuilt only
	// when the set of shapes with geometry changes.
	uint32_t signature = 0;
	for (const ShapeEntry &entry : shapes) {
		if (entry.geometry_type) {
			CollisionShape3D *cs = ObjectDB::get_instance<CollisionShape3D>(entry.shape_node);
			signature = hash_murmur3_one_64(cs ? uint64_t(cs->get_shape()->get_instance_id()) : 0, signature);
		}
	}
	const bool rebuild = signature != geometry_signature;
	geometry_signature = signature;
	if (rebuild) {
		geometry.clear();
	}
	HashMap<ObjectID, Vector2i> placed;
	int cursor = 0; // in vec4s: where the next block goes (the same places as when it was built)
	for (ShapeEntry &entry : shapes) {
		if (!entry.geometry_type) {
			continue;
		}
		CollisionShape3D *cs = ObjectDB::get_instance<CollisionShape3D>(entry.shape_node);
		const ObjectID shape_id = cs->get_shape()->get_instance_id();
		if (const Vector2i *at = placed.getptr(shape_id)) {
			entry.geometry_a = at->x;
			entry.geometry_b = at->y;
			continue;
		}
		const GeometryBlock &block = geometry_blocks[shape_id];
		const int base = cursor;
		if (block.type == 4) {
			entry.geometry_a = base;
			entry.geometry_b = block.planes.size() / 4;
			cursor += block.planes.size() / 4;
			if (rebuild) {
				geometry.append_array(block.planes);
			}
		} else {
			// The mesh's node and triangle indices are relative: the shader adds these bases.
			entry.geometry_a = base;
			entry.geometry_b = base + block.nodes.size() / 4;
			cursor += (block.nodes.size() + block.triangles.size()) / 4;
			if (rebuild) {
				geometry.append_array(block.nodes);
				geometry.append_array(block.triangles);
			}
		}
		placed.insert(shape_id, Vector2i(entry.geometry_a, entry.geometry_b));
	}
	if (rebuild) {
		geometry_version++;
	}
}

// Where shape p_index is and its local bounds (an emissive's box); false when it can't be traced.
bool PXTGI3D::_entry_frame(int p_index, Transform3D &r_to_world, AABB &r_local) const {
	float record[PhysXGIEffect::PRIMITIVE_FLOATS];
	AABB world;
	if (!_write_entry(p_index, record, world)) {
		return false;
	}
	// The record holds world-to-local rows: their inverse is the shape's frame; its local box from the world one.
	Transform3D to_local;
	for (int r = 0; r < 3; r++) {
		to_local.basis.rows[r] = Vector3(record[r * 4 + 0], record[r * 4 + 1], record[r * 4 + 2]);
		to_local.origin[r] = record[r * 4 + 3];
	}
	r_to_world = to_local.affine_inverse();
	r_local = to_local.xform(world);
	const ShapeEntry &entry = shapes[p_index];
	if (entry.geometry_type) {
		r_local = entry.local_bounds;
	} else {
		const float type = record[12];
		const Vector3 e(record[13], record[14], record[15]);
		r_local = type == 1.0f ? AABB(-Vector3(e.x, e.x, e.x), Vector3(2, 2, 2) * e.x) : AABB(-e, e * 2.0f);
	}
	return true;
}

// Where shape p_index is and what it is (type, extents), from its node; false when it can't be traced (gone, hidden,
// disabled).
bool PXTGI3D::_shape_frame(int p_index, Transform3D &r_to_world, float &r_type, Vector3 &r_extents) const {
	const ShapeEntry &entry = shapes[p_index];
	float type = 0.0f;
	Vector3 extents;
	Transform3D to_world;
	if (entry.mesh_proxy) {
		MeshInstance3D *mesh = ObjectDB::get_instance<MeshInstance3D>(entry.shape_node);
		if (mesh == nullptr || !mesh->is_visible_in_tree() || mesh->get_mesh().is_null()) {
			return false;
		}
		const AABB bounds = mesh->get_aabb();
		extents = bounds.size * 0.5f;
		to_world = mesh->get_global_transform() * Transform3D(Basis(), bounds.get_center());
	} else {
		CollisionShape3D *cs = ObjectDB::get_instance<CollisionShape3D>(entry.shape_node);
		if (cs == nullptr || cs->is_disabled() || !cs->is_inside_tree() || cs->get_shape().is_null()) {
			return false;
		}
		to_world = cs->get_global_transform();
		Ref<Shape3D> shape = cs->get_shape();
		if (entry.geometry_type) {
			type = float(entry.geometry_type);
		} else if (Ref<BoxShape3D> box = shape; box.is_valid()) {
			extents = box->get_size() * 0.5f;
		} else if (Ref<SphereShape3D> sphere = shape; sphere.is_valid()) {
			type = 1.0f;
			extents = Vector3(sphere->get_radius(), 0, 0);
		} else if (Ref<CapsuleShape3D> capsule = shape; capsule.is_valid()) {
			type = 2.0f;
			extents = Vector3(capsule->get_radius(), capsule->get_height() * 0.5f, capsule->get_radius());
		} else if (Ref<CylinderShape3D> cylinder = shape; cylinder.is_valid()) {
			type = 3.0f;
			extents = Vector3(cylinder->get_radius(), cylinder->get_height() * 0.5f, cylinder->get_radius());
		} else {
			return false;
		}
	}
	r_to_world = to_world;
	r_type = type;
	r_extents = extents;
	return true;
}

// Shape p_index's GPU record and world bounds, placed at p_to_world.
void PXTGI3D::_pack_entry(int p_index, const Transform3D &p_to_world, float p_type, const Vector3 &p_extents, float *r_out, AABB &r_bounds) const {
	const ShapeEntry &entry = shapes[p_index];
	const float type = p_type;
	const Vector3 extents = p_extents;
	const Transform3D to_local = p_to_world.affine_inverse();
	const AABB local_bounds = entry.geometry_type ? entry.local_bounds : (type == 1.0f ? AABB(-Vector3(extents.x, extents.x, extents.x), Vector3(2, 2, 2) * extents.x) : AABB(-extents, extents * 2.0f));
	r_bounds = p_to_world.xform(local_bounds);
	float *p = r_out;
	for (int r = 0; r < 3; r++) {
		p[r * 4 + 0] = to_local.basis.rows[r].x;
		p[r * 4 + 1] = to_local.basis.rows[r].y;
		p[r * 4 + 2] = to_local.basis.rows[r].z;
		p[r * 4 + 3] = to_local.origin[r];
	}
	p[12] = type;
	p[13] = extents.x;
	p[14] = extents.y;
	p[15] = extents.z;
	if (entry.geometry_type) {
		store_int(&p[13], entry.geometry_a);
		store_int(&p[14], entry.geometry_b);
	}
	p[16] = entry.albedo.r;
	p[17] = entry.albedo.g;
	p[18] = entry.albedo.b;
	p[19] = 1.0f;
	p[20] = entry.emission.r;
	p[21] = entry.emission.g;
	p[22] = entry.emission.b;
	p[23] = 0.0f;
}

// Shape p_index's GPU record and world bounds, from its node; false when it can't be traced.
bool PXTGI3D::_write_entry(int p_index, float *r_out, AABB &r_bounds) const {
	Transform3D to_world;
	float type;
	Vector3 extents;
	if (!_shape_frame(p_index, to_world, type, extents)) {
		return false;
	}
	_pack_entry(p_index, to_world, type, extents, r_out, r_bounds);
	return true;
}

// As _write_entry, also noting where the shape sits on its body, for the frames its body moves.
bool PXTGI3D::_refresh_entry(int p_index, float *r_out, AABB &r_bounds) {
	ShapeEntry &entry = shapes[p_index];
	entry.posed = false;
	if (!_shape_frame(p_index, entry.in_body, entry.type, entry.extents)) {
		return false;
	}
	const Transform3D to_world = entry.in_body;
	_pack_entry(p_index, to_world, entry.type, entry.extents, r_out, r_bounds);
	if (!entry.mesh_proxy) {
		const Node3D *body = ObjectDB::get_instance<CollisionShape3D>(entry.shape_node)->get_parent_node_3d();
		if (body) {
			entry.in_body = body->get_global_transform().affine_inverse() * to_world;
			entry.posed = true;
		}
	}
	return true;
}

// The bodies PhysX reported moving in the last step: only their shapes are rewritten (null when the world isn't
// simulated by PhysX: then every shape is checked).
static GodotPhysXSpace3D *physx_space(Node3D *p_node) {
	GodotPhysXServer3D *server = GodotPhysXServer3D::get_singleton();
	if (server == nullptr || !p_node->is_inside_world()) {
		return nullptr;
	}
	return server->get_space(p_node->get_world_3d()->get_space());
}

// The shapes' records and their tree, refreshed for what changed: a rescan that finds different shapes rebuilds the
// tree; otherwise only the shapes on bodies PhysX moved are rewritten, placed from the bodies' poses (everything every
// 30 frames from the nodes, to catch bodies moved outside the simulation and shapes edited, hidden or disabled) and the
// tree is refit around them, rebuilt after a while of motion to stay tight.
void PXTGI3D::_update_primitives(PhysXGIEffect::Snapshot &r_snapshot) {
	constexpr int PF = PhysXGIEffect::PRIMITIVE_FLOATS;
	bool rebuild = false;
	if (--rescan_in <= 0) {
		const uint32_t before = shape_signature;
		_rescan();
		rescan_in = 60;
		rebuild = shape_signature != before;
	}
	const int n = shapes.size();
	LocalVector<int> changed;
	if (rebuild || entry_floats.size() != uint32_t(n * PF)) {
		entry_floats.resize(n * PF);
		entry_bounds.resize(n);
		entry_valid.resize(n);
		for (int i = 0; i < n; i++) {
			entry_valid[i] = _refresh_entry(i, &entry_floats[i * PF], entry_bounds[i]);
		}
		rebuild = true;
	} else {
		// Each candidate with the PhysX body that moved it (its shapes placed from the body's pose), or none (read from
		// its node).
		LocalVector<Pair<int, const GodotPhysXBody3D *>> candidates;
		GodotPhysXSpace3D *space = physx_space(this);
		if (--verify_in <= 0 || space == nullptr) {
			verify_in = 30;
			for (int i = 0; i < n; i++) {
				candidates.push_back({ i, nullptr });
			}
		} else {
			for (GodotPhysXBody3D *body : space->get_awake_bodies()) {
				if (const LocalVector<int> *list = body_entries.getptr(body->get_instance_id())) {
					for (int i : *list) {
						candidates.push_back({ i, body });
					}
				}
			}
			for (int i : proxy_entries) {
				candidates.push_back({ i, nullptr });
			}
		}
		float scratch[PF];
		AABB bounds;
		for (const Pair<int, const GodotPhysXBody3D *> &c : candidates) {
			const int i = c.first;
			const ShapeEntry &entry = shapes[i];
			bool valid;
			if (c.second && entry.posed && entry_valid[i]) {
				_pack_entry(i, c.second->get_transform() * entry.in_body, entry.type, entry.extents, scratch, bounds);
				valid = true;
			} else {
				valid = _refresh_entry(i, scratch, bounds);
			}
			if (valid != bool(entry_valid[i])) {
				rebuild = true;
			}
			entry_valid[i] = valid;
			if (valid && memcmp(scratch, &entry_floats[i * PF], sizeof(scratch)) != 0) {
				memcpy(&entry_floats[i * PF], scratch, sizeof(scratch));
				entry_bounds[i] = bounds;
				changed.push_back(i);
			}
		}
		if (rebuild) {
			for (int i = 0; i < n; i++) {
				entry_valid[i] = _refresh_entry(i, &entry_floats[i * PF], entry_bounds[i]);
			}
		}
	}
	// A refit keeps the grouping of the last build: as a pile tumbles, its boxes loosen and overlap and every ray walks
	// more of the tree (a collapsing pile doubled the trace). Rebuilt every quarter second of motion.
	if (!changed.is_empty() && ++moves_since_build > 15) {
		rebuild = true;
	}

	if (rebuild) {
		LocalVector<BuildItem> items;
		for (int i = 0; i < n; i++) {
			if (entry_valid[i]) {
				items.push_back({ entry_bounds[i], entry_bounds[i].get_center(), i });
			}
		}
		ordered_floats.clear();
		tree_nodes.clear();
		leaf_order.clear();
		if (!items.is_empty()) {
			build_bvh(items, 0, items.size(), entry_floats.ptr(), tree_nodes, ordered_floats, leaf_order);
		}
		entry_slot.resize(n);
		for (int i = 0; i < n; i++) {
			entry_slot[i] = -1;
		}
		for (uint32_t k = 0; k < leaf_order.size(); k++) {
			entry_slot[leaf_order[k]] = k;
		}
		moves_since_build = 0;
	} else if (!changed.is_empty()) {
		// Refit: the moved records into their leaf slots, then every node's box from its children, bottom up (a
		// node's children come after it).
		for (int i : changed) {
			if (entry_slot[i] >= 0) {
				memcpy(&ordered_floats[entry_slot[i] * PF], &entry_floats[i * PF], PF * sizeof(float));
			}
		}
		auto node_box = [&](int p_node) {
			const float *o = &tree_nodes[p_node * PhysXGIEffect::NODE_FLOATS];
			return AABB(Vector3(o[0], o[1], o[2]), Vector3(o[4] - o[0], o[5] - o[1], o[6] - o[2]));
		};
		for (int node = int(tree_nodes.size() / PhysXGIEffect::NODE_FLOATS) - 1; node >= 0; node--) {
			float *o = &tree_nodes[node * PhysXGIEffect::NODE_FLOATS];
			int a;
			int count;
			memcpy(&a, &o[3], sizeof(int));
			memcpy(&count, &o[7], sizeof(int));
			AABB box;
			if (count > 0) {
				box = entry_bounds[leaf_order[a]];
				for (int k = 1; k < count; k++) {
					box.merge_with(entry_bounds[leaf_order[a + k]]);
				}
			} else {
				box = node_box(node + 1).merge(node_box(a));
			}
			o[0] = box.position.x;
			o[1] = box.position.y;
			o[2] = box.position.z;
			o[4] = box.position.x + box.size.x;
			o[5] = box.position.y + box.size.y;
			o[6] = box.position.z + box.size.z;
		}
	}

	if (rebuild || !changed.is_empty()) {
		published_primitives.resize(ordered_floats.size());
		if (ordered_floats.size()) {
			memcpy(published_primitives.ptrw(), ordered_floats.ptr(), ordered_floats.size() * sizeof(float));
		}
		published_nodes.resize(tree_nodes.size());
		if (tree_nodes.size()) {
			memcpy(published_nodes.ptrw(), tree_nodes.ptr(), tree_nodes.size() * sizeof(float));
		}
		published_count = leaf_order.size();
		primitives_version++;
	}
	r_snapshot.primitives = published_primitives;
	r_snapshot.primitive_count = published_count;
	r_snapshot.nodes = published_nodes;
	r_snapshot.node_count = published_nodes.size() / PhysXGIEffect::NODE_FLOATS;
	r_snapshot.primitives_version = primitives_version;
}

void PXTGI3D::_publish() {
	if (!is_visible_in_tree()) {
		PhysXGIEffect::remove_settings(get_instance_id());
		return;
	}
	PhysXGIEffect::Snapshot s;
	s.intensity = intensity;
	s.max_distance = max_distance;
	s.world_distance = world_ray_distance;
	s.debug = debug;
	s.multi_bounce = multi_bounce;
	s.rays_per_texel = rays_per_texel;
	s.reflection_mode = reflection_mode;
	s.reflection_cutoff = reflection_cutoff;

	_update_primitives(s);
	{
		const Camera3D *camera = get_viewport() ? get_viewport()->get_camera_3d() : nullptr;
		const Vector3 eye = camera ? camera->get_global_position() : get_global_position();
		LocalVector<Pair<float, int>> near;
		for (int i : emissive_entries) {
			Transform3D frame;
			AABB local;
			if (_entry_frame(i, frame, local)) {
				near.push_back(Pair<float, int>(frame.xform(local.get_center()).distance_squared_to(eye), i));
			}
		}
		near.sort_custom<PairSort<float, int>>();
		s.emissive_count = 0;
		for (const Pair<float, int> &e : near) {
			if (s.emissive_count == PhysXGIEffect::Snapshot::MAX_EMISSIVES) {
				break;
			}
			Transform3D frame;
			AABB local;
			_entry_frame(e.second, frame, local);
			const int k = s.emissive_count++;
			const Vector3 half = local.size * 0.5f;
			s.emissive_center[k] = frame.xform(local.get_center());
			for (int a = 0; a < 3; a++) {
				Vector3 axis;
				axis[a] = half[a];
				s.emissive_axis[k][a] = frame.basis.xform(axis);
			}
			s.emissive_color[k] = shapes[e.second].emission;
		}
	}
	s.geometry = geometry;
	s.geometry_version = geometry_version;

	DirectionalLight3D *light = ObjectDB::get_instance<DirectionalLight3D>(sun);
	if (light && light->is_visible_in_tree()) {
		s.sun_direction = light->get_global_transform().basis.get_column(2).normalized();
		s.sun_color = light->get_color().srgb_to_linear() * light->get_param(Light3D::PARAM_ENERGY);
	} else {
		s.sun_color = Color(0, 0, 0);
	}
	s.omni_count = 0;
	for (const ObjectID &id : omnis) {
		OmniLight3D *omni = ObjectDB::get_instance<OmniLight3D>(id);
		if (omni == nullptr || !omni->is_visible_in_tree()) {
			continue;
		}
		const int i = s.omni_count++;
		s.omni_position[i] = omni->get_global_position();
		s.omni_inv_range[i] = 1.0f / MAX(omni->get_param(Light3D::PARAM_RANGE), 0.001f);
		s.omni_color[i] = omni->get_color().srgb_to_linear() * omni->get_param(Light3D::PARAM_ENERGY);
		s.omni_attenuation[i] = omni->get_param(Light3D::PARAM_ATTENUATION);
	}
	s.spot_count = 0;
	for (const ObjectID &id : spots) {
		SpotLight3D *spot = ObjectDB::get_instance<SpotLight3D>(id);
		if (spot == nullptr || !spot->is_visible_in_tree()) {
			continue;
		}
		const int i = s.spot_count++;
		const Transform3D xf = spot->get_global_transform();
		s.spot_position[i] = xf.origin;
		s.spot_inv_range[i] = 1.0f / MAX(spot->get_param(Light3D::PARAM_RANGE), 0.001f);
		s.spot_direction[i] = -xf.basis.get_column(2).normalized();
		s.spot_cos_angle[i] = Math::cos(Math::deg_to_rad(spot->get_param(Light3D::PARAM_SPOT_ANGLE)));
		s.spot_color[i] = spot->get_color().srgb_to_linear() * spot->get_param(Light3D::PARAM_ENERGY);
		s.spot_attenuation[i] = spot->get_param(Light3D::PARAM_ATTENUATION);
		s.spot_cone_attenuation[i] = 1.0f / MAX(spot->get_param(Light3D::PARAM_SPOT_ATTENUATION), 0.001f);
	}
	PhysXGIEffect::set_settings(get_instance_id(), s);
	// SDFGI is only the switch that makes surfaces read the GI buffer: on, at its cheapest (one cascade of 8 m cells,
	// so a moving camera rarely makes it re-voxelize).
	Camera3D *camera = get_viewport() ? get_viewport()->get_camera_3d() : nullptr;
	Ref<Environment> env = camera && camera->get_environment().is_valid() ? camera->get_environment() : get_world_3d()->get_environment();
	if (environment.is_valid() && environment != env) {
		environment->set_sdfgi_enabled(environment->is_sdfgi_enabled());
	}
	environment = env;
	if (env.is_valid()) {
		if (env->get_ambient_source() == Environment::AMBIENT_SOURCE_COLOR) {
			s.ambient = env->get_ambient_light_color().srgb_to_linear() * env->get_ambient_light_energy();
			PhysXGIEffect::set_settings(get_instance_id(), s);
		}
		RenderingServer::get_singleton()->environment_set_sdfgi(env->get_rid(), true, 1, 8.0f, RSE::ENV_SDFGI_Y_SCALE_100_PERCENT, false, 0.0f, false, 1.0f, 1.1f, 1.1f);
	}

	// Anything that changes the light (shapes moved, lights, settings) means converging again.
	uint32_t h = hash_murmur3_one_64(s.primitives_version);
	h = hash_murmur3_one_float(s.intensity, h);
	h = hash_murmur3_one_float(s.max_distance, h);
	h = hash_murmur3_one_float(s.world_distance, h);
	h = hash_murmur3_one_32(s.debug | (s.reflection_mode << 4) | (s.multi_bounce ? 256 : 0) | (s.rays_per_texel << 10), h);
	h = hash_murmur3_one_float(s.sun_direction.x + s.sun_direction.y * 3.1f + s.sun_direction.z * 7.3f + s.sun_color.get_luminance(), h);
	for (int i = 0; i < s.spot_count; i++) {
		h = hash_murmur3_one_float(s.spot_position[i].x + s.spot_direction[i].y * 3.1f + s.spot_direction[i].z * 7.3f + s.spot_color[i].get_luminance(), h);
	}
	for (int i = 0; i < s.omni_count; i++) {
		h = hash_murmur3_one_float(s.omni_position[i].x + s.omni_position[i].y * 3.1f + s.omni_position[i].z * 7.3f + s.omni_color[i].get_luminance(), h);
	}
	if (h != last_hash) {
		last_hash = h;
		PhysXGIEffect::unsettle();
	}
}

void PXTGI3D::_notification(int p_what) {
	if (RenderingServer::get_singleton()->get_current_rendering_method() != "forward_plus") {
		return;
	}
	switch (p_what) {
		case NOTIFICATION_ENTER_WORLD: {
			if (is_visible_in_tree()) {
				_activate();
			}
		} break;
		case NOTIFICATION_EXIT_WORLD: {
			_deactivate();
		} break;
		case NOTIFICATION_VISIBILITY_CHANGED: {
			// Hidden, the node steps aside entirely, as if it had left the world: Godot's own GI is back.
			if (is_visible_in_tree() && is_inside_world()) {
				_activate();
			} else {
				_deactivate();
			}
		} break;
		case NOTIFICATION_INTERNAL_PROCESS: {
			if (!attached_world.is_valid()) {
				return;
			}
			_publish();
			PhysXGIEffect::install(get_world_3d().ptr(), get_viewport() ? get_viewport()->get_camera_3d() : nullptr);
#ifdef TOOLS_ENABLED
			// The editor draws only when something changes: keep it drawing until the GI has converged.
			if (Engine::get_singleton()->is_editor_hint() && PhysXGIEffect::settling()) {
				Main::force_redraw();
			}
#endif
		} break;
	}
}

PackedStringArray PXTGI3D::get_configuration_warnings() const {
	PackedStringArray warnings = Node3D::get_configuration_warnings();
	if (RenderingServer::get_singleton()->get_current_rendering_method() != "forward_plus") {
		warnings.push_back(RTR("PXTGI3D only works with the Forward+ renderer: with Mobile or Compatibility it does nothing."));
	}
	return warnings;
}

void PXTGI3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_intensity", "intensity"), &PXTGI3D::set_intensity);
	ClassDB::bind_method(D_METHOD("get_intensity"), &PXTGI3D::get_intensity);
	ClassDB::bind_method(D_METHOD("set_max_distance", "distance"), &PXTGI3D::set_max_distance);
	ClassDB::bind_method(D_METHOD("get_max_distance"), &PXTGI3D::get_max_distance);
	ClassDB::bind_method(D_METHOD("set_world_ray_distance", "distance"), &PXTGI3D::set_world_ray_distance);
	ClassDB::bind_method(D_METHOD("get_world_ray_distance"), &PXTGI3D::get_world_ray_distance);
	ClassDB::bind_method(D_METHOD("set_rays_per_texel", "rays"), &PXTGI3D::set_rays_per_texel);
	ClassDB::bind_method(D_METHOD("get_rays_per_texel"), &PXTGI3D::get_rays_per_texel);
	ClassDB::bind_method(D_METHOD("set_multi_bounce", "enabled"), &PXTGI3D::set_multi_bounce);
	ClassDB::bind_method(D_METHOD("get_multi_bounce"), &PXTGI3D::get_multi_bounce);
	ClassDB::bind_method(D_METHOD("set_reflection_mode", "mode"), &PXTGI3D::set_reflection_mode);
	ClassDB::bind_method(D_METHOD("get_reflection_mode"), &PXTGI3D::get_reflection_mode);
	ClassDB::bind_method(D_METHOD("set_reflection_roughness_cutoff", "roughness"), &PXTGI3D::set_reflection_roughness_cutoff);
	ClassDB::bind_method(D_METHOD("get_reflection_roughness_cutoff"), &PXTGI3D::get_reflection_roughness_cutoff);
	ClassDB::bind_method(D_METHOD("set_debug", "mode"), &PXTGI3D::set_debug);
	ClassDB::bind_method(D_METHOD("get_debug"), &PXTGI3D::get_debug);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "intensity", PROPERTY_HINT_RANGE, "0,4,0.01,or_greater"), "set_intensity", "get_intensity");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "max_distance", PROPERTY_HINT_RANGE, "0.1,20,0.1,or_greater"), "set_max_distance", "get_max_distance");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "world_ray_distance", PROPERTY_HINT_RANGE, "1,100,0.5,or_greater,suffix:m"), "set_world_ray_distance", "get_world_ray_distance");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "rays_per_texel", PROPERTY_HINT_RANGE, "1,4,1"), "set_rays_per_texel", "get_rays_per_texel");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "multi_bounce"), "set_multi_bounce", "get_multi_bounce");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "reflection_mode", PROPERTY_HINT_ENUM, "Godot,Screen Space,Traced"), "set_reflection_mode", "get_reflection_mode");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "reflection_roughness_cutoff", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_reflection_roughness_cutoff", "get_reflection_roughness_cutoff");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "debug", PROPERTY_HINT_ENUM, "Off,Last Frame,Round Trip,World Only,World Hits,World Position"), "set_debug", "get_debug");
}
