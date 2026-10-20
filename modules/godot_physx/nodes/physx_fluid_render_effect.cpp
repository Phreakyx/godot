/**************************************************************************/
/*  physx_fluid_render_effect.cpp                                         */
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

#include "physx_fluid_render_effect.h"

#include "physx_compositor_effects.h"

#include "core/object/callable_mp.h"
#include "scene/3d/camera_3d.h"
#include "scene/resources/3d/world_3d.h"
#include "servers/rendering/renderer_rd/storage_rd/render_scene_buffers_rd.h"
#include "servers/rendering/renderer_rd/storage_rd/render_scene_data_rd.h"
#include "servers/rendering/rendering_device.h"
#include "servers/rendering/rendering_device_binds.h"
#include "servers/rendering/rendering_server.h"
#include "servers/rendering/storage/render_data.h"

Mutex PhysXFluidRenderEffect::mutex;
HashMap<ObjectID, PhysXFluidRenderEffect::Snapshot> PhysXFluidRenderEffect::fluids;
HashMap<ObjectID, int> PhysXFluidRenderEffect::worlds;
HashMap<ObjectID, PhysXFluidRenderEffect::MergedDepth> PhysXFluidRenderEffect::merged_depths;

namespace {

Ref<PhysXFluidRenderEffect> shared_effect;

enum Pass {
	PASS_CLEAR,
	PASS_SPLAT,
	PASS_FOAM,
	PASS_RESOLVE,
	PASS_FILTER,
	PASS_COPY,
	PASS_COMPOSITE,
	PASS_MAX,
};

// Shared by every pass (binding 0), spliced in at @PARAMS@.
const char *PARAMS = R"(
layout(set = 0, binding = 0, std140) uniform Params {
	mat4 view; // world -> view
	mat4 proj; // view -> clip, as the scene's depth was drawn
	mat4 inv_proj;
	vec4 absorption; // rgb: per meter of fluid; a: the depth jump the smoothing keeps (m)
	vec4 sky; // rgb: what the surface reflects
	vec4 light_dir; // xyz: view space, toward the light
	vec4 light_color; // a: the thickness (m) under which the fluid fades out
	vec4 misc; // x: refraction, y: smoothing radius (m), z: max splat / filter radius (px), w: thickness units per meter
	ivec4 size; // xy: the surface buffers (the frame / frame.z), z: particles
	ivec4 frame; // xy: the frame, z: frame pixels per surface texel, each way
	vec4 surface; // rgb: the body colour (linear), a: opacity per meter of fluid (0: clear)
	vec4 shading; // x: highlight power, y: highlight strength
	vec4 emission; // rgb, added
	vec4 foam; // rgb: foam colour (linear), a: the most a foam patch covers
	ivec4 foam_n; // x: foam particles
} p;

// Eye-space depth (meters in front of the camera) of a point on the pixel's ray.
vec3 view_pos(vec2 px, float z) {
	vec2 ndc = (px + 0.5) / vec2(p.size.xy) * 2.0 - 1.0;
	vec4 v = p.inv_proj * vec4(ndc, 0.5, 1.0);
	vec3 dir = v.xyz / v.w;
	return dir * (z / -dir.z);
}
)";

const char *SHADERS[PASS_MAX] = {
	// PASS_CLEAR
	R"(
#[compute]
#version 450
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;
@PARAMS@
layout(set = 0, binding = 1, r32ui) uniform restrict writeonly uimage2D depth_img;
layout(set = 0, binding = 2, r32ui) uniform restrict writeonly uimage2D thick_img;
layout(set = 0, binding = 3, r32ui) uniform restrict writeonly uimage2D foam_depth_img;
layout(set = 0, binding = 4, r32ui) uniform restrict writeonly uimage2D foam_img;
void main() {
	ivec2 px = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(px, p.size.xy))) {
		return;
	}
	imageStore(depth_img, px, uvec4(0x7f800000u)); // +inf: no fluid
	imageStore(thick_img, px, uvec4(0u));
	imageStore(foam_depth_img, px, uvec4(0x7f800000u));
	imageStore(foam_img, px, uvec4(0u));
}
)",
	// PASS_SPLAT: each particle draws a sphere's front depth (atomic min of the
	// float bits -- positive floats order like their bits) and its thickness.
	R"(
#[compute]
#version 450
layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;
@PARAMS@
layout(set = 0, binding = 1, r32ui) uniform restrict uimage2D depth_img;
layout(set = 0, binding = 2, r32ui) uniform restrict uimage2D thick_img;
layout(set = 0, binding = 3, std430) restrict readonly buffer Points {
	vec4 pts[];
};
layout(set = 0, binding = 4) uniform sampler2D scene_depth;

float scene_eye_depth(ivec2 px) {
	ivec2 fpx = min(px * p.frame.z + p.frame.z / 2, p.frame.xy - 1);
	float d = texelFetch(scene_depth, fpx, 0).r;
	vec2 ndc = (vec2(fpx) + 0.5) / vec2(p.frame.xy) * 2.0 - 1.0;
	vec4 v = p.inv_proj * vec4(ndc, d, 1.0);
	return -v.z / v.w;
}

void main() {
	uint i = gl_GlobalInvocationID.x;
	if (i >= uint(p.size.z)) {
		return;
	}
	vec4 pt = pts[i];
	float r = pt.w;
	vec4 vp = p.view * vec4(pt.xyz, 1.0);
	float z = -vp.z;
	if (z < r + 0.01) {
		return;
	}
	vec4 c = p.proj * vp;
	vec2 cpx = (c.xy / c.w * 0.5 + 0.5) * vec2(p.size.xy);
	float rpx = max(r * abs(p.proj[1][1]) * 0.5 * float(p.size.y) / z, 0.5);
	int R = int(ceil(min(rpx, p.misc.z)));
	ivec2 cc = ivec2(floor(cpx));
	for (int y = -R; y <= R; y++) {
		for (int x = -R; x <= R; x++) {
			ivec2 px = cc + ivec2(x, y);
			if (any(lessThan(px, ivec2(0))) || any(greaterThanEqual(px, p.size.xy))) {
				continue;
			}
			vec2 d = (vec2(px) + 0.5 - cpx) / rpx;
			float d2 = dot(d, d);
			if (d2 > 1.0) {
				continue;
			}
			float h = sqrt(1.0 - d2);
			float zs = z - r * h;
			if (zs > scene_eye_depth(px)) {
				continue; // behind the scene's own geometry
			}
			imageAtomicMin(depth_img, px, floatBitsToUint(zs));
			imageAtomicAdd(thick_img, px, uint(2.0 * r * h * p.misc.w));
		}
	}
}
)",
	// PASS_FOAM: each foam particle a soft disc -- its front depth (atomic min)
	// and a density that falls off to its edge (thousandths, summed).
	R"(
#[compute]
#version 450
layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;
@PARAMS@
layout(set = 0, binding = 1, r32ui) uniform restrict uimage2D foam_depth_img;
layout(set = 0, binding = 2, r32ui) uniform restrict uimage2D foam_img;
layout(set = 0, binding = 3, std430) restrict readonly buffer Points {
	vec4 pts[];
};
layout(set = 0, binding = 4) uniform sampler2D scene_depth;

float scene_eye_depth(ivec2 px) {
	ivec2 fpx = min(px * p.frame.z + p.frame.z / 2, p.frame.xy - 1);
	float d = texelFetch(scene_depth, fpx, 0).r;
	vec2 ndc = (vec2(fpx) + 0.5) / vec2(p.frame.xy) * 2.0 - 1.0;
	vec4 v = p.inv_proj * vec4(ndc, d, 1.0);
	return -v.z / v.w;
}

void main() {
	uint i = gl_GlobalInvocationID.x;
	if (i >= uint(p.foam_n.x)) {
		return;
	}
	vec4 pt = pts[i];
	float r = pt.w;
	vec4 vp = p.view * vec4(pt.xyz, 1.0);
	float z = -vp.z;
	if (z < r + 0.01) {
		return;
	}
	vec4 c = p.proj * vp;
	vec2 cpx = (c.xy / c.w * 0.5 + 0.5) * vec2(p.size.xy);
	float rpx = max(r * abs(p.proj[1][1]) * 0.5 * float(p.size.y) / z, 0.75);
	int R = int(ceil(min(rpx, p.misc.z)));
	ivec2 cc = ivec2(floor(cpx));
	for (int y = -R; y <= R; y++) {
		for (int x = -R; x <= R; x++) {
			ivec2 px = cc + ivec2(x, y);
			if (any(lessThan(px, ivec2(0))) || any(greaterThanEqual(px, p.size.xy))) {
				continue;
			}
			vec2 d = (vec2(px) + 0.5 - cpx) / rpx;
			float d2 = dot(d, d);
			if (d2 > 1.0 || z - r > scene_eye_depth(px)) {
				continue;
			}
			imageAtomicMin(foam_depth_img, px, floatBitsToUint(z - r * sqrt(1.0 - d2)));
			imageAtomicAdd(foam_img, px, uint((1.0 - d2) * 1000.0));
		}
	}
}
)",
	// PASS_RESOLVE: the splatted depth bits as floats (+inf where empty).
	R"(
#[compute]
#version 450
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;
@PARAMS@
layout(set = 0, binding = 1, r32ui) uniform restrict readonly uimage2D depth_img;
layout(set = 0, binding = 2, r32f) uniform restrict writeonly image2D dst;
void main() {
	ivec2 px = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(px, p.size.xy))) {
		return;
	}
	imageStore(dst, px, vec4(uintBitsToFloat(imageLoad(depth_img, px).r)));
}
)",
	// PASS_FILTER: a 2D bilateral blur of the depth over up to 8 px each way, run
	// a few times (small blurs add up to a wide one without a sparse sampling
	// grid's screen-door pattern) -- the spheres melt into one surface, while a
	// jump in depth (the stream in front of the pool, the fluid's outline) is
	// kept. (A separable blur leaves axis-aligned blocks.)
	R"(
#[compute]
#version 450
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;
@PARAMS@
layout(push_constant, std430) uniform Push {
	ivec2 dir;
	ivec2 pad;
} pc;
layout(set = 0, binding = 1, r32f) uniform restrict readonly image2D src;
layout(set = 0, binding = 2, r32f) uniform restrict writeonly image2D dst;
void main() {
	ivec2 px = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(px, p.size.xy))) {
		return;
	}
	float z = imageLoad(src, px).r;
	if (z > 1e30) {
		imageStore(dst, px, vec4(z));
		return;
	}
	float rpx = p.misc.y * abs(p.proj[1][1]) * 0.5 * float(p.size.y) / z;
	int steps = int(clamp(ceil(rpx), 1.0, 8.0));
	float stride = 1.0;
	float sigma_s = float(steps) * 0.5;
	float sigma_z = p.absorption.a; // depth jump kept, meters
	float sum = 0.0;
	float wsum = 0.0;
	for (int j = -steps; j <= steps; j++) {
		for (int i = -steps; i <= steps; i++) {
			vec2 o = vec2(i, j) * stride;
			ivec2 q = clamp(px + ivec2(round(o)), ivec2(0), p.size.xy - 1);
			float zq = imageLoad(src, q).r;
			if (zq > 1e30) {
				continue;
			}
			float dz = zq - z;
			float w = exp(-dot(o, o) / (2.0 * sigma_s * sigma_s)) * exp(-dz * dz / (2.0 * sigma_z * sigma_z));
			sum += zq * w;
			wsum += w;
		}
	}
	imageStore(dst, px, vec4(sum / max(wsum, 1e-6)));
}
)",
	// PASS_COPY: the frame as it is, for the refraction to read while the
	// composite writes.
	R"(
#[compute]
#version 450
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;
@PARAMS@
layout(set = 0, binding = 1, rgba16f) uniform restrict readonly image2D src;
layout(set = 0, binding = 2, rgba16f) uniform restrict writeonly image2D dst;
void main() {
	ivec2 px = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(px, p.frame.xy))) {
		return;
	}
	imageStore(dst, px, imageLoad(src, px));
}
)",
	// PASS_COMPOSITE (full resolution): shade the smoothed surface into the
	// frame. At a reduced resolution each frame pixel blends the 2x2 surface
	// texels around it that hold fluid -- the share that do is its coverage,
	// a soft edge against the background -- and is tested against the scene's
	// own full-resolution depth, so where the fluid meets geometry the edge
	// stays sharp.
	R"(
#[compute]
#version 450
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;
@PARAMS@
layout(set = 0, binding = 1, r32f) uniform restrict readonly image2D zimg;
layout(set = 0, binding = 2, r32ui) uniform restrict readonly uimage2D thick_img;
layout(set = 0, binding = 3) uniform sampler2D scene_color;
layout(set = 0, binding = 4, rgba16f) uniform restrict writeonly image2D color_img;
layout(set = 0, binding = 5) uniform sampler2D scene_depth;
layout(set = 0, binding = 6, r32ui) uniform restrict readonly uimage2D foam_depth_img;
layout(set = 0, binding = 7, r32ui) uniform restrict readonly uimage2D foam_img;
layout(set = 0, binding = 8, r32f) uniform restrict writeonly image2D merged_depth;

float zat(ivec2 q, float fallback) {
	q = clamp(q, ivec2(0), p.size.xy - 1);
	float z = imageLoad(zimg, q).r;
	return z > 1e30 ? fallback : z;
}

void main() {
	ivec2 fpx = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(fpx, p.frame.xy))) {
		return;
	}
	vec2 uv = (vec2(fpx) + 0.5) / vec2(p.frame.xy);
	float sd = texelFetch(scene_depth, fpx, 0).r;
	vec4 sv = p.inv_proj * vec4(uv * 2.0 - 1.0, sd, 1.0);
	float scene_z = -sv.z / sv.w;
	float out_depth = sd; // what Flow marches against
	float surface_z = 1e30; // the fluid's surface, where it draws solid enough
	vec3 base = textureLod(scene_color, uv, 0.0).rgb;
	vec3 col = base;
	bool drawn = false;

	vec2 l = (vec2(fpx) + 0.5) / float(p.frame.z) - 0.5;
	ivec2 b = ivec2(floor(l));
	vec2 t = l - vec2(b);
	ivec2 qs[4] = ivec2[](b, b + ivec2(1, 0), b + ivec2(0, 1), b + ivec2(1, 1));
	float ws[4] = float[]((1.0 - t.x) * (1.0 - t.y), t.x * (1.0 - t.y), (1.0 - t.x) * t.y, t.x * t.y);
	float coverage = 0.0;
	float z = 0.0;
	float T = 0.0;
	float best = -1.0;
	ivec2 px = b;
	float foam = 0.0;
	float foam_z = 1e30;
	for (int k = 0; k < 4; k++) {
		ivec2 q = clamp(qs[k], ivec2(0), p.size.xy - 1);
		float fk = float(imageLoad(foam_img, q).r) / 1000.0;
		if (fk > 0.0) {
			foam += fk * ws[k];
			foam_z = min(foam_z, uintBitsToFloat(imageLoad(foam_depth_img, q).r));
		}
		float zk = imageLoad(zimg, q).r;
		if (zk > 1e30) {
			continue;
		}
		coverage += ws[k];
		z += zk * ws[k];
		T += float(imageLoad(thick_img, q).r) / p.misc.w * ws[k];
		if (ws[k] > best) {
			best = ws[k];
			px = q;
		}
	}
	// In front of the scene's geometry at this exact pixel?
	if (coverage > 0.0 && z / coverage <= scene_z) {
		z /= coverage;
		T /= coverage;
		// Normal from the depth around the nearest surface texel: per axis, the
		// neighbour on the flatter side, so silhouettes don't smear.
		float zc = imageLoad(zimg, px).r;
		vec3 P = view_pos(vec2(px), zc);
		float zr = zat(px + ivec2(1, 0), zc);
		float zl = zat(px - ivec2(1, 0), zc);
		float zu = zat(px + ivec2(0, 1), zc);
		float zd = zat(px - ivec2(0, 1), zc);
		vec3 ddx = abs(zr - zc) < abs(zc - zl) ? view_pos(vec2(px + ivec2(1, 0)), zr) - P : P - view_pos(vec2(px - ivec2(1, 0)), zl);
		vec3 ddy = abs(zu - zc) < abs(zc - zd) ? view_pos(vec2(px + ivec2(0, 1)), zu) - P : P - view_pos(vec2(px - ivec2(0, 1)), zd);
		vec3 n = normalize(cross(ddx, ddy));
		vec3 V = normalize(-P);
		if (dot(n, V) < 0.0) {
			n = -n;
		}
		vec2 ruv = clamp(uv + n.xy * p.misc.x * min(T, 1.0), vec2(0.0), vec2(1.0));
		vec3 refracted = textureLod(scene_color, ruv, 0.0).rgb * exp(-p.absorption.rgb * T);
		vec3 L = normalize(p.light_dir.xyz);
		// An opaque-ish fluid (the material's alpha) shows its own lit colour more
		// the thicker it is.
		float body = 1.0 - exp(-p.surface.a * T);
		vec3 lit = p.surface.rgb * (0.3 + 0.7 * max(dot(n, L), 0.0) * p.light_dir.w * p.light_color.rgb);
		refracted = mix(refracted, lit, body);
		float F = 0.02 + 0.98 * pow(1.0 - max(dot(n, V), 0.0), 5.0);
		float spec = pow(max(dot(n, normalize(L + V)), 0.0), p.shading.x) * p.shading.y * p.light_dir.w;
		vec3 shaded = mix(refracted, p.sky.rgb, F) + spec * p.light_color.rgb + p.emission.rgb;
		// Thin fluid (a lone droplet, a sheet's edge) fades into the scene rather
		// than drawing as a hard bubble; so does the edge of the coverage.
		float a = smoothstep(0.0, p.light_color.a, T) * coverage;
		col = mix(base, shaded, a);
		drawn = true;
		if (a > 0.5) {
			// The surface, on this pixel's ray, as the depth buffer stores it.
			vec4 v = p.inv_proj * vec4(uv * 2.0 - 1.0, 0.5, 1.0);
			vec3 dir = v.xyz / v.w;
			vec4 c = p.proj * vec4(dir * (z / -dir.z), 1.0);
			out_depth = c.z / c.w;
			surface_z = z;
		}
	}
	// Foam over the surface; foam under it fades with how deep it is.
	if (foam > 0.0 && foam_z <= scene_z) {
		float under = max(foam_z - surface_z, 0.0);
		float fa = p.foam.a * (1.0 - exp(-foam * 1.5)) * exp(-under * 12.0);
		col = mix(col, p.foam.rgb * (0.65 + 0.35 * min(p.light_dir.w, 1.0)), fa);
		drawn = true;
	}
	if (drawn) {
		imageStore(color_img, fpx, vec4(col, 1.0));
	}
	imageStore(merged_depth, fpx, vec4(out_depth));
}
)",
};

const char *PASS_NAMES[PASS_MAX] = { "clear", "splat", "foam", "resolve", "filter", "copy", "composite" };

void put_matrix(float *r_dst, const Projection &p_m) {
	for (int c = 0; c < 4; c++) {
		for (int r = 0; r < 4; r++) {
			r_dst[c * 4 + r] = (float)p_m.columns[c][r];
		}
	}
}

} // namespace

void PhysXFluidRenderEffect::set_fluid(const ObjectID &p_fluid, const Snapshot &p_snapshot) {
	MutexLock lock(mutex);
	fluids[p_fluid] = p_snapshot;
}

void PhysXFluidRenderEffect::set_foam(const ObjectID &p_fluid, const PackedFloat32Array &p_foam) {
	MutexLock lock(mutex);
	Snapshot *s = fluids.getptr(p_fluid);
	if (s != nullptr) {
		s->foam = p_foam;
	}
}

void PhysXFluidRenderEffect::remove_fluid(const ObjectID &p_fluid) {
	MutexLock lock(mutex);
	fluids.erase(p_fluid);
}

void PhysXFluidRenderEffect::attach_world(World3D *p_world) {
	ERR_FAIL_NULL(p_world);
	worlds[p_world->get_instance_id()]++;
}

void PhysXFluidRenderEffect::detach_world(const ObjectID &p_world) {
	int *count = worlds.getptr(p_world);
	if (count == nullptr || --(*count) > 0) {
		return;
	}
	worlds.erase(p_world);
	PhysXCompositorEffects::uninstall(p_world, get_shared());
}

void PhysXFluidRenderEffect::install(World3D *p_world, Camera3D *p_camera) {
	ERR_FAIL_NULL(p_world);
	ERR_FAIL_COND(!worlds.has(p_world->get_instance_id()));
	PhysXCompositorEffects::install(p_world, p_camera, get_shared(), PhysXCompositorEffects::ORDER_FLUID);
}

RID PhysXFluidRenderEffect::take_merged_depth(const ObjectID &p_buffers) {
	MutexLock lock(mutex);
	const MergedDepth *m = merged_depths.getptr(p_buffers);
	if (m == nullptr) {
		return RID();
	}
	RenderingDevice *rd = RenderingServer::get_singleton()->get_rendering_device();
	const RID depth = (rd != nullptr && m->frame == rd->get_frames_drawn()) ? m->texture : RID();
	merged_depths.erase(p_buffers);
	return depth;
}

Ref<PhysXFluidRenderEffect> PhysXFluidRenderEffect::get_shared() {
	if (shared_effect.is_null()) {
		shared_effect.instantiate();
	}
	return shared_effect;
}

void PhysXFluidRenderEffect::free_shared() {
	shared_effect.unref();
}

PhysXFluidRenderEffect::PhysXFluidRenderEffect() {
	set_effect_callback_type(EFFECT_CALLBACK_TYPE_POST_TRANSPARENT);
	set_access_resolved_color(true);
	set_access_resolved_depth(true);
	RenderingServer::get_singleton()->compositor_effect_set_callback(get_rid(), RSE::COMPOSITOR_EFFECT_CALLBACK_TYPE_POST_TRANSPARENT, callable_mp(this, &PhysXFluidRenderEffect::_render));
}

PhysXFluidRenderEffect::~PhysXFluidRenderEffect() {
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
	for (RID r : { linear_sampler, point_sampler, params_buffer, points_buffer, foam_buffer }) {
		if (r.is_valid()) {
			rd->free_rid(r);
		}
	}
}

bool PhysXFluidRenderEffect::_ensure(RenderingDevice *p_rd) {
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
		const String source = String(SHADERS[i]).replace("@PARAMS@", PARAMS);
		ERR_FAIL_COND_V_MSG(file->parse_versions_from_text(source) != OK, false, vformat("PhysXParticleFluid3D: screen-space %s shader failed to compile.", PASS_NAMES[i]));
		Ref<RDShaderSPIRV> spirv = file->get_spirv();
		ERR_FAIL_COND_V(spirv.is_null(), false);
		const String err = spirv->get_stage_compile_error(RD::SHADER_STAGE_COMPUTE);
		ERR_FAIL_COND_V_MSG(!err.is_empty(), false, vformat("PhysXParticleFluid3D: screen-space %s shader: %s", PASS_NAMES[i], err));
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
	RD::SamplerState point;
	point_sampler = p_rd->sampler_create(point);
	params_buffer = p_rd->uniform_buffer_create(PARAMS_FLOATS * 4);
	failed = false;
	return true;
}

void PhysXFluidRenderEffect::_free_targets(RenderingDevice *p_rd) {
	for (RID *r : { &depth_u, &thick_u, &foam_depth_u, &foam_u, &smooth_a, &smooth_b, &scene_copy, &merged_depth }) {
		if (r->is_valid()) {
			p_rd->free_rid(*r);
			*r = RID();
		}
	}
	size = Size2i();
	frame_size = Size2i();
}

void PhysXFluidRenderEffect::_ensure_targets(RenderingDevice *p_rd, const Size2i &p_size, const Size2i &p_frame) {
	if (p_size == size && p_frame == frame_size && depth_u.is_valid()) {
		return;
	}
	_free_targets(p_rd);
	size = p_size;
	frame_size = p_frame;
	RD::TextureFormat tf;
	tf.width = p_size.x;
	tf.height = p_size.y;
	tf.usage_bits = RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_SAMPLING_BIT;
	tf.format = RD::DATA_FORMAT_R32_UINT;
	tf.usage_bits |= RD::TEXTURE_USAGE_STORAGE_ATOMIC_BIT;
	depth_u = p_rd->texture_create(tf, RD::TextureView());
	thick_u = p_rd->texture_create(tf, RD::TextureView());
	foam_depth_u = p_rd->texture_create(tf, RD::TextureView());
	foam_u = p_rd->texture_create(tf, RD::TextureView());
	tf.usage_bits = RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_SAMPLING_BIT;
	tf.format = RD::DATA_FORMAT_R32_SFLOAT;
	smooth_a = p_rd->texture_create(tf, RD::TextureView());
	smooth_b = p_rd->texture_create(tf, RD::TextureView());
	tf.format = RD::DATA_FORMAT_R16G16B16A16_SFLOAT;
	tf.width = p_frame.x;
	tf.height = p_frame.y;
	scene_copy = p_rd->texture_create(tf, RD::TextureView());
	tf.format = RD::DATA_FORMAT_R32_SFLOAT;
	merged_depth = p_rd->texture_create(tf, RD::TextureView());
}

void PhysXFluidRenderEffect::_dispatch(RenderingDevice *p_rd, int p_pass, const Vector<RD::Uniform> &p_uniforms, uint32_t p_x, uint32_t p_y, const void *p_push, uint32_t p_push_size) {
	const RID set = p_rd->uniform_set_create(p_uniforms, shaders[p_pass], 0);
	ERR_FAIL_COND(!set.is_valid());
	// Its own compute list per pass: the next pass then sees this one's writes
	// (as the Flow light scattering's two passes do).
	const RD::ComputeListID list = p_rd->compute_list_begin();
	p_rd->compute_list_bind_compute_pipeline(list, pipelines[p_pass]);
	p_rd->compute_list_bind_uniform_set(list, set, 0);
	if (p_push != nullptr) {
		p_rd->compute_list_set_push_constant(list, p_push, p_push_size);
	}
	p_rd->compute_list_dispatch(list, p_x, p_y, 1);
	p_rd->compute_list_end();
	p_rd->free_rid(set);
}

void PhysXFluidRenderEffect::_render(int p_callback_type, const RenderData *p_render_data) {
	// Every fluid's particles become one surface (streams that meet merge);
	// the shading comes from the fluid nearest the camera.
	PackedFloat32Array points;
	PackedFloat32Array foam;
	Snapshot look;
	{
		MutexLock lock(mutex);
		if (fluids.is_empty()) {
			return;
		}
		int total = 0;
		int foam_total = 0;
		for (const KeyValue<ObjectID, Snapshot> &kv : fluids) {
			total += kv.value.points.size();
			foam_total += kv.value.foam.size();
		}
		if (total == 0 && foam_total == 0) {
			return;
		}
		points.resize(MAX(total, 4)); // a placeholder point keeps the buffer valid
		foam.resize(MAX(foam_total, 4));
		float *w = points.ptrw();
		float *fw = foam.ptrw();
		for (const KeyValue<ObjectID, Snapshot> &kv : fluids) {
			memcpy(w, kv.value.points.ptr(), kv.value.points.size() * sizeof(float));
			w += kv.value.points.size();
			memcpy(fw, kv.value.foam.ptr(), kv.value.foam.size() * sizeof(float));
			fw += kv.value.foam.size();
		}
		if (total == 0) {
			points.resize(0);
		}
		if (foam_total == 0) {
			foam.resize(0);
		}
		look = fluids.begin()->value;
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
	// Nearest fluid's look.
	const Transform3D cam = scene->get_cam_transform();
	{
		MutexLock lock(mutex);
		real_t best = 1e30;
		for (const KeyValue<ObjectID, Snapshot> &kv : fluids) {
			const real_t d = cam.origin.distance_squared_to(kv.value.center);
			if (d < best) {
				best = d;
				look = kv.value;
			}
		}
	}

	const int div = look.resolution_divisor;
	const Size2i low((frame.x + div - 1) / div, (frame.y + div - 1) / div);
	_ensure_targets(rd, low, frame);

	const uint32_t count = points.size() / 4;
	const uint32_t foam_count = foam.size() / 4;
	auto upload = [&](RID &r_buffer, uint32_t &r_capacity, const PackedFloat32Array &p_data, uint32_t p_count) {
		if (p_count > r_capacity || !r_buffer.is_valid()) {
			if (r_buffer.is_valid()) {
				rd->free_rid(r_buffer);
			}
			r_capacity = MAX(MAX(p_count, 1u), r_capacity * 2);
			r_buffer = rd->storage_buffer_create(r_capacity * 16);
		}
		if (p_count > 0) {
			rd->buffer_update(r_buffer, 0, p_count * 16, p_data.ptr());
		}
	};
	upload(points_buffer, points_capacity, points, count);
	upload(foam_buffer, foam_capacity, foam, foam_count);

	const Projection proj = scene->get_cam_projection();
	const Projection view = Projection(cam.affine_inverse());
	float f[PARAMS_FLOATS] = {};
	put_matrix(f + 0, view);
	put_matrix(f + 16, proj);
	put_matrix(f + 32, proj.inverse());
	f[48] = look.absorption.r;
	f[49] = look.absorption.g;
	f[50] = look.absorption.b;
	f[51] = look.depth_threshold;
	f[52] = look.sky.r;
	f[53] = look.sky.g;
	f[54] = look.sky.b;
	const Vector3 to_light = (cam.basis.inverse().xform(-look.light_direction)).normalized();
	f[56] = to_light.x;
	f[57] = to_light.y;
	f[58] = to_light.z;
	f[59] = look.light_color.a;
	f[60] = look.light_color.r;
	f[61] = look.light_color.g;
	f[62] = look.light_color.b;
	f[63] = look.fade_thickness;
	f[64] = look.refraction;
	f[65] = look.smoothing;
	f[66] = 24.0f; // max splat / filter radius, px
	f[67] = 1000.0f; // thickness: millimeters in the uint buffer
	const int32_t ints[8] = { low.x, low.y, (int32_t)count, 0, frame.x, frame.y, div, 0 };
	memcpy(f + 68, ints, sizeof(ints));
	f[76] = look.surface.r;
	f[77] = look.surface.g;
	f[78] = look.surface.b;
	f[79] = look.surface.a;
	f[80] = look.highlight_power;
	f[81] = look.highlight_strength;
	f[84] = look.emission.r;
	f[85] = look.emission.g;
	f[86] = look.emission.b;
	f[88] = look.foam_color.r;
	f[89] = look.foam_color.g;
	f[90] = look.foam_color.b;
	f[91] = look.foam_color.a;
	const int32_t foam_ints[4] = { (int32_t)foam_count, 0, 0, 0 };
	memcpy(f + 92, foam_ints, sizeof(foam_ints));
	rd->buffer_update(params_buffer, 0, sizeof(f), f);

	const uint32_t gx = (low.x + 7) / 8;
	const uint32_t gy = (low.y + 7) / 8;
	const uint32_t fx = (frame.x + 7) / 8;
	const uint32_t fy = (frame.y + 7) / 8;
	const RD::Uniform params(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 0, params_buffer);
	_dispatch(rd, PASS_CLEAR, { params, RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 1, depth_u), RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 2, thick_u), RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 3, foam_depth_u), RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 4, foam_u) }, gx, gy, nullptr, 0);
	if (foam_count > 0) {
		_dispatch(rd, PASS_FOAM, { params, RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 1, foam_depth_u), RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 2, foam_u), RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 3, foam_buffer), RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 4, Vector<RID>({ point_sampler, depth })) }, (foam_count + 63) / 64, 1, nullptr, 0);
	}
	if (count > 0) {
		_dispatch(rd, PASS_SPLAT, { params, RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 1, depth_u), RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 2, thick_u), RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 3, points_buffer), RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 4, Vector<RID>({ point_sampler, depth })) }, (count + 63) / 64, 1, nullptr, 0);
	}
	_dispatch(rd, PASS_RESOLVE, { params, RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 1, depth_u), RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 2, smooth_b) }, gx, gy, nullptr, 0);
	const int32_t no_dir[4] = { 0, 0, 0, 0 };
	for (int i = 0; i < 3; i++) { // b -> a -> b -> a
		const RID from = (i % 2 == 0) ? smooth_b : smooth_a;
		const RID to = (i % 2 == 0) ? smooth_a : smooth_b;
		_dispatch(rd, PASS_FILTER, { params, RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 1, from), RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 2, to) }, gx, gy, no_dir, sizeof(no_dir));
	}
	_dispatch(rd, PASS_COPY, { params, RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 1, color), RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 2, scene_copy) }, fx, fy, nullptr, 0);
	_dispatch(rd, PASS_COMPOSITE, { params, RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 1, smooth_a), RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 2, thick_u), RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 3, Vector<RID>({ linear_sampler, scene_copy })), RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 4, color), RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 5, Vector<RID>({ point_sampler, depth })), RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 6, foam_depth_u), RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 7, foam_u), RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 8, merged_depth) }, fx, fy, nullptr, 0);
	// For PhysXFlowRenderEffect, which runs next (see PhysXCompositorEffects).
	MutexLock lock(mutex);
	merged_depths[buffers->get_instance_id()] = MergedDepth{ merged_depth, rd->get_frames_drawn() };
}
