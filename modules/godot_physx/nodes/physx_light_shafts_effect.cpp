/**************************************************************************/
/*  physx_light_shafts_effect.cpp                                         */
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

#include "physx_light_shafts_effect.h"

#include "physx_compositor_effects.h"

#include "core/object/callable_mp.h"
#include "scene/3d/camera_3d.h"
#include "scene/resources/3d/world_3d.h"
#include "servers/rendering/renderer_rd/storage_rd/render_scene_buffers_rd.h"
#include "servers/rendering/renderer_rd/storage_rd/render_scene_data_rd.h"
#include "servers/rendering/rendering_device_binds.h"
#include "servers/rendering/rendering_server.h"
#include "servers/rendering/storage/render_data.h"

Mutex PhysXLightShaftsEffect::mutex;
HashMap<ObjectID, PhysXLightShaftsEffect::Snapshot> PhysXLightShaftsEffect::shafts;
HashMap<ObjectID, int> PhysXLightShaftsEffect::worlds;

namespace {

Ref<PhysXLightShaftsEffect> shared_effect;

// Each blur pass takes this many samples toward the sun; three passes, each
// reaching SAMPLES times further than the last, smear over SAMPLES^3.
constexpr int SAMPLES = 16;

// Shared by every pass, spliced in at @PARAMS@ (a push constant: 128 bytes).
const char *PARAMS = R"(
layout(push_constant, std430) uniform Params {
	mat4 inv_proj; // clip -> view, as the scene's depth was drawn
	vec4 sun; // xy: the sun on screen (uv, may be off it), z: fade, w: width / height
	vec4 bloom; // rgb: tint * scale (black: no bloom), a: threshold
	vec4 misc; // x: occlusion strength, y: this blur pass's reach (uv), z: max brightness, w: occlusion depth range (m)
	ivec4 size; // xy: the half-resolution buffers, zw: the frame
} p;

// How near the sun a point on screen is, 1 at it to 0 three quarters of a
// screen height away, easing out (no edge where it ends).
float near_sun(vec2 uv) {
	vec2 d = (p.sun.xy - uv) * vec2(p.sun.w, 1.0);
	float t = clamp(1.0 - length(d) / 0.75, 0.0, 1.0);
	return t * t;
}
)";

const char *SHADERS[3] = {
	// PASS_PREPARE: how far the light gets through each (half-res) pixel, and the
	// bright sky there to bloom.
	R"(
#[compute]
#version 450
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;
@PARAMS@
layout(set = 0, binding = 0) uniform sampler2D color_tex;
layout(set = 0, binding = 1) uniform sampler2D depth_tex;
layout(set = 0, binding = 2, rgba16f) uniform restrict writeonly image2D dst;
void main() {
	ivec2 px = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(px, p.size.xy))) {
		return;
	}
	vec2 uv = (vec2(px) + 0.5) / vec2(p.size.xy);
	// The least open of the frame's pixels under this one: a thin occluder
	// (a branch, a railing) still blocks at half resolution.
	ivec2 f = ivec2(px * 2);
	float open = 1.0;
	for (int i = 0; i < 4; i++) {
		ivec2 q = min(f + ivec2(i & 1, i >> 1), p.size.zw - 1);
		float z = texelFetch(depth_tex, q, 0).r;
		// Reverse Z: 0 is the far plane (the sky). Geometry counts as more
		// open the further away it is, out to the depth range.
		vec2 ndc = (vec2(q) + 0.5) / vec2(p.size.zw) * 2.0 - 1.0;
		vec4 v = p.inv_proj * vec4(ndc, z, 1.0);
		float dist = z <= 0.0 ? 1e30 : length(v.xyz / v.w);
		open = min(open, clamp(dist / max(p.misc.w, 1e-3), 0.0, 1.0));
	}
	vec3 c = texture(color_tex, uv).rgb;
	float lum = max(dot(c, vec3(0.2126, 0.7152, 0.0722)), 1e-5);
	vec3 bright = c * (clamp(lum - p.bloom.a, 0.0, p.misc.z) / lum);
	// Only the light near the sun streams, and not from the screen's very
	// edges (it would smear in as if from off screen).
	float edge = clamp(64.0 * uv.x * (1.0 - uv.x) * uv.y * (1.0 - uv.y), 0.0, 1.0);
	imageStore(dst, px, vec4(bright * open * near_sun(uv) * edge, open));
}
)",
	// PASS_BLUR: a radial blur toward the sun, reaching p.misc.y.
	R"(
#[compute]
#version 450
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;
@PARAMS@
layout(set = 0, binding = 0) uniform sampler2D src;
layout(set = 0, binding = 1, rgba16f) uniform restrict writeonly image2D dst;
void main() {
	ivec2 px = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(px, p.size.xy))) {
		return;
	}
	vec2 uv = (vec2(px) + 0.5) / vec2(p.size.xy);
	vec2 to_sun = p.sun.xy - uv;
	float dist = length(to_sun);
	// Never past the sun itself.
	vec2 step_uv = dist > 1e-5 ? to_sun / dist * min(p.misc.y, dist) / float(@SAMPLES@) : vec2(0.0);
	vec4 sum = vec4(0.0);
	for (int i = 0; i < @SAMPLES@; i++) {
		sum += texture(src, uv + step_uv * float(i));
	}
	imageStore(dst, px, sum / float(@SAMPLES@));
}
)",
	// PASS_COMPOSITE: the rays onto the frame.
	R"(
#[compute]
#version 450
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;
@PARAMS@
layout(set = 0, binding = 0) uniform sampler2D shafts;
layout(set = 0, binding = 1, rgba16f) uniform restrict image2D color;
void main() {
	ivec2 px = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(px, p.size.zw))) {
		return;
	}
	vec2 uv = (vec2(px) + 0.5) / vec2(p.size.zw);
	vec4 s = texture(shafts, uv);
	vec4 c = imageLoad(color, px);
	// Darkened where the way to the sun is blocked, near the sun only (further
	// out the rays have spread too thin to show).
	float shade = mix(1.0, mix(1.0 - p.misc.x, 1.0, s.a), near_sun(uv) * p.sun.z);
	c.rgb = c.rgb * shade + s.rgb * p.bloom.rgb * p.sun.z;
	imageStore(color, px, c);
}
)",
};

const char *PASS_NAMES[3] = { "prepare", "blur", "composite" };

struct Push {
	float inv_proj[16];
	float sun[4];
	float bloom[4];
	float misc[4];
	int32_t size[4];
};
static_assert(sizeof(Push) == 128);

} // namespace

void PhysXLightShaftsEffect::set_shafts(const ObjectID &p_node, const Snapshot &p_snapshot) {
	MutexLock lock(mutex);
	shafts[p_node] = p_snapshot;
}

void PhysXLightShaftsEffect::remove_shafts(const ObjectID &p_node) {
	MutexLock lock(mutex);
	shafts.erase(p_node);
}

void PhysXLightShaftsEffect::attach_world(World3D *p_world) {
	ERR_FAIL_NULL(p_world);
	worlds[p_world->get_instance_id()]++;
}

void PhysXLightShaftsEffect::detach_world(const ObjectID &p_world) {
	int *count = worlds.getptr(p_world);
	if (count == nullptr || --(*count) > 0) {
		return;
	}
	worlds.erase(p_world);
	PhysXCompositorEffects::uninstall(p_world, get_shared());
}

void PhysXLightShaftsEffect::install(World3D *p_world, Camera3D *p_camera) {
	ERR_FAIL_NULL(p_world);
	ERR_FAIL_COND(!worlds.has(p_world->get_instance_id()));
	PhysXCompositorEffects::install(p_world, p_camera, get_shared(), PhysXCompositorEffects::ORDER_SHAFTS);
}

Ref<PhysXLightShaftsEffect> PhysXLightShaftsEffect::get_shared() {
	if (shared_effect.is_null()) {
		shared_effect.instantiate();
	}
	return shared_effect;
}

void PhysXLightShaftsEffect::free_shared() {
	shared_effect.unref();
}

PhysXLightShaftsEffect::PhysXLightShaftsEffect() {
	set_effect_callback_type(EFFECT_CALLBACK_TYPE_POST_TRANSPARENT);
	set_access_resolved_color(true);
	set_access_resolved_depth(true);
	RenderingServer::get_singleton()->compositor_effect_set_callback(get_rid(), RSE::COMPOSITOR_EFFECT_CALLBACK_TYPE_POST_TRANSPARENT, callable_mp(this, &PhysXLightShaftsEffect::_render));
}

PhysXLightShaftsEffect::~PhysXLightShaftsEffect() {
	RenderingDevice *rd = RenderingServer::get_singleton() ? RenderingServer::get_singleton()->get_rendering_device() : nullptr;
	if (rd == nullptr) {
		return;
	}
	_free_targets(rd);
	for (int i = 0; i < PASS_MAX; i++) {
		if (pipelines[i].is_valid()) {
			rd->free_rid(pipelines[i]);
		}
		if (shaders[i].is_valid()) {
			rd->free_rid(shaders[i]);
		}
	}
	if (linear_sampler.is_valid()) {
		rd->free_rid(linear_sampler);
	}
}

bool PhysXLightShaftsEffect::_ensure(RenderingDevice *p_rd) {
	if (pipelines[PASS_COMPOSITE].is_valid()) {
		return true;
	}
	if (failed) {
		return false;
	}
	failed = true;
	for (int i = 0; i < PASS_MAX; i++) {
		Ref<RDShaderFile> file;
		file.instantiate();
		const String source = String(SHADERS[i]).replace("@PARAMS@", PARAMS).replace("@SAMPLES@", itos(SAMPLES));
		ERR_FAIL_COND_V_MSG(file->parse_versions_from_text(source) != OK, false, vformat("PhysXLightShafts3D: %s shader failed to compile.", PASS_NAMES[i]));
		Ref<RDShaderSPIRV> spirv = file->get_spirv();
		ERR_FAIL_COND_V(spirv.is_null(), false);
		const String err = spirv->get_stage_compile_error(RD::SHADER_STAGE_COMPUTE);
		ERR_FAIL_COND_V_MSG(!err.is_empty(), false, vformat("PhysXLightShafts3D: %s shader: %s", PASS_NAMES[i], err));
		shaders[i] = p_rd->shader_create_from_spirv(file->get_spirv_stages());
		ERR_FAIL_COND_V(!shaders[i].is_valid(), false);
		pipelines[i] = p_rd->compute_pipeline_create(shaders[i]);
		ERR_FAIL_COND_V(!pipelines[i].is_valid(), false);
	}
	RD::SamplerState linear;
	linear.mag_filter = RD::SAMPLER_FILTER_LINEAR;
	linear.min_filter = RD::SAMPLER_FILTER_LINEAR;
	linear.repeat_u = RD::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE;
	linear.repeat_v = RD::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE;
	linear_sampler = p_rd->sampler_create(linear);
	failed = false;
	return true;
}

void PhysXLightShaftsEffect::_free_targets(RenderingDevice *p_rd) {
	for (RID *r : { &half_a, &half_b }) {
		if (r->is_valid()) {
			p_rd->free_rid(*r);
			*r = RID();
		}
	}
	half_size = Size2i();
}

void PhysXLightShaftsEffect::_ensure_targets(RenderingDevice *p_rd, const Size2i &p_size) {
	if (p_size == half_size && half_a.is_valid()) {
		return;
	}
	_free_targets(p_rd);
	half_size = p_size;
	RD::TextureFormat tf;
	tf.width = p_size.x;
	tf.height = p_size.y;
	tf.format = RD::DATA_FORMAT_R16G16B16A16_SFLOAT;
	tf.usage_bits = RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_SAMPLING_BIT;
	half_a = p_rd->texture_create(tf, RD::TextureView());
	half_b = p_rd->texture_create(tf, RD::TextureView());
}

void PhysXLightShaftsEffect::_dispatch(RenderingDevice *p_rd, int p_pass, const Vector<RD::Uniform> &p_uniforms, const Size2i &p_size, const void *p_push, uint32_t p_push_size) {
	const RID set = p_rd->uniform_set_create(p_uniforms, shaders[p_pass], 0);
	ERR_FAIL_COND(!set.is_valid());
	// Its own compute list per pass: the next pass then sees this one's writes.
	const RD::ComputeListID list = p_rd->compute_list_begin();
	p_rd->compute_list_bind_compute_pipeline(list, pipelines[p_pass]);
	p_rd->compute_list_bind_uniform_set(list, set, 0);
	p_rd->compute_list_set_push_constant(list, p_push, p_push_size);
	p_rd->compute_list_dispatch(list, (p_size.x + 7) / 8, (p_size.y + 7) / 8, 1);
	p_rd->compute_list_end();
	p_rd->free_rid(set);
}

void PhysXLightShaftsEffect::_render(int p_callback_type, const RenderData *p_render_data) {
	Snapshot s;
	{
		MutexLock lock(mutex);
		if (shafts.is_empty()) {
			return;
		}
		s = shafts.begin()->value;
	}
	if (p_render_data == nullptr) {
		return;
	}
	RenderSceneBuffersRD *buffers = Object::cast_to<RenderSceneBuffersRD>(p_render_data->get_render_scene_buffers().ptr());
	RenderSceneDataRD *scene = Object::cast_to<RenderSceneDataRD>(p_render_data->get_render_scene_data());
	RenderingDevice *rd = RenderingServer::get_singleton()->get_rendering_device();
	if (buffers == nullptr || scene == nullptr || rd == nullptr || !_ensure(rd)) {
		return;
	}
	const Size2i frame = buffers->get_internal_size();
	const RID color = buffers->get_internal_texture();
	const RID depth = buffers->get_depth_texture();
	if (frame.x <= 0 || frame.y <= 0 || !color.is_valid() || !depth.is_valid()) {
		return;
	}
	const Projection proj = scene->get_cam_projection();
	if (proj.is_orthogonal() || s.light_direction.is_zero_approx()) {
		return; // a sun at infinity has no place on an orthographic screen
	}

	// The sun's place on screen: the direction toward it, projected.
	const Vector3 to_sun = scene->get_cam_transform().basis.xform_inv(-s.light_direction.normalized());
	const Vector4 clip = proj.xform(Vector4(to_sun.x, to_sun.y, to_sun.z, 0.0));
	if (clip.w <= 1e-4) {
		return; // behind the camera
	}
	const Vector2 sun_uv = Vector2(clip.x, clip.y) / clip.w * 0.5 + Vector2(0.5, 0.5);
	// Faded out as it leaves the screen.
	const real_t outside = MAX(MAX(MAX(-sun_uv.x, sun_uv.x - 1.0), MAX(-sun_uv.y, sun_uv.y - 1.0)), 0.0);
	const real_t fade = s.edge_fade > 0.0f ? 1.0 - Math::smoothstep((real_t)0.0, (real_t)s.edge_fade, outside) : (outside > 0.0 ? 0.0 : 1.0);
	if (fade <= 0.0) {
		return;
	}

	const Size2i half((frame.x + 1) / 2, (frame.y + 1) / 2);
	_ensure_targets(rd, half);

	Push push = {};
	const Projection inv_proj = proj.inverse();
	for (int c = 0; c < 4; c++) {
		for (int r = 0; r < 4; r++) {
			push.inv_proj[c * 4 + r] = (float)inv_proj.columns[c][r];
		}
	}
	push.sun[0] = (float)sun_uv.x;
	push.sun[1] = (float)sun_uv.y;
	push.sun[2] = (float)fade;
	push.sun[3] = (float)frame.x / (float)frame.y;
	push.bloom[0] = s.bloom_color.r;
	push.bloom[1] = s.bloom_color.g;
	push.bloom[2] = s.bloom_color.b;
	push.bloom[3] = s.bloom_threshold;
	push.misc[0] = s.occlusion_strength;
	push.misc[2] = s.bloom_max_brightness;
	push.misc[3] = s.occlusion_depth_range;
	push.size[0] = half.x;
	push.size[1] = half.y;
	push.size[2] = frame.x;
	push.size[3] = frame.y;

	_dispatch(rd, PASS_PREPARE, { RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, Vector<RID>({ linear_sampler, color })), RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 1, Vector<RID>({ linear_sampler, depth })), RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 2, half_a) }, half, &push, sizeof(push));
	// Three passes, each reaching SAMPLES times further: the last reaches the
	// full ray length.
	RID src = half_a;
	RID dst = half_b;
	for (int pass = 0; pass < 3; pass++) {
		push.misc[1] = s.ray_length / (float)(pass == 0 ? SAMPLES * SAMPLES : (pass == 1 ? SAMPLES : 1));
		_dispatch(rd, PASS_BLUR, { RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, Vector<RID>({ linear_sampler, src })), RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 1, dst) }, half, &push, sizeof(push));
		SWAP(src, dst);
	}
	_dispatch(rd, PASS_COMPOSITE, { RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, Vector<RID>({ linear_sampler, src })), RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 1, color) }, frame, &push, sizeof(push));
}
