/**************************************************************************/
/*  flow_scatter_rd.cpp                                                   */
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


#include "flow_scatter_rd.h"

#include "servers/rendering/rendering_device.h"
#include "servers/rendering/rendering_device_binds.h"

namespace {

const char *SCATTER_SHADER = R"(
#[compute]
#version 450

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

// Flow's density atlas (r: temperature, b: sun light, a: smoke) and its
// sparse table; see NvFlowShader.hlsli for the lookups ported below.
layout(set = 0, binding = 0) uniform sampler3D density_tex;
layout(set = 0, binding = 1, std430) restrict readonly buffer Table {
	uint table_data[];
};
layout(set = 0, binding = 2) uniform sampler2D depth_tex;
layout(set = 0, binding = 3, rgba16f) uniform restrict image2D color_img;

struct Light {
	vec4 position_range;
	vec4 color_decay;
	vec4 direction_cutoff;
	vec4 spot;
};
layout(set = 0, binding = 4, std430) restrict readonly buffer Lights {
	Light lights[];
};

layout(set = 0, binding = 5, std140) uniform Params {
	mat4 inv_view_projection; // clip -> world meters
	vec4 camera; // xyz meters; w: units per meter
	uvec4 level[10]; // NvFlowSparseLevelParams
	vec4 block_size; // Flow units; w: step scale (cells)
	ivec4 counts; // x: layerAndLevel, y: lights, z: shadow steps, w: max steps
	vec4 world_min; // Flow units
	vec4 world_max;
	vec4 albedo; // rgb; w: strength
	vec4 misc; // x: sigma per meter per unit smoke, y: anisotropy, z: shadow reach (m)
	ivec4 size;
} p;

#define BLOCK_DIM_BITS (p.level[1].xyz)
#define TABLE_DIM_LESS_ONE (p.level[2].xyz)
#define TABLE_DIM_BITS_X (p.level[3].x)
#define TABLE_DIM_BITS_XY (p.level[3].y)
#define LOCATION_OFFSET (p.level[3].w)
#define BLOCK_LEVEL_OFFSET_GLOBAL (p.level[4].z)
#define DIM_INV (uintBitsToFloat(p.level[7].xyz))

const uint NO_BLOCK = 0xFFFFFFFFu;

uint location_to_block(ivec4 loc) {
	uvec3 bucket = uvec3(loc.xyz) & TABLE_DIM_LESS_ONE;
	uint bucket1d = (bucket.z << TABLE_DIM_BITS_XY) | (bucket.y << TABLE_DIM_BITS_X) | bucket.x;
	uint range_begin = table_data[(bucket1d << 1u) + 0u];
	uint range_end = table_data[(bucket1d << 1u) + 1u];
	for (uint b = range_begin; b < range_end; b++) {
		uint i = (b << 2u) + LOCATION_OFFSET;
		if (int(table_data[i + 0u]) == loc.x && int(table_data[i + 1u]) == loc.y && int(table_data[i + 2u]) == loc.z && int(table_data[i + 3u]) == loc.w) {
			return b;
		}
	}
	return NO_BLOCK;
}

// One block lookup remembered (consecutive samples mostly share a block).
struct Accessor {
	ivec4 location;
	ivec3 offset; // virtual cell -> atlas texel
	bool valid;
};

void accessor_update(inout Accessor a, ivec4 loc) {
	if (loc == a.location) {
		return;
	}
	a.location = loc;
	uint b = location_to_block(loc);
	a.valid = false;
	if (b != NO_BLOCK) {
		uint v = table_data[b + BLOCK_LEVEL_OFFSET_GLOBAL];
		a.valid = (v >> 31u) != 0u;
		ivec3 base = ivec3(int((v << 1u) | 1u) & 0xFFF, int((v >> 10u) | 1u) & 0x7FF, int((v >> 20u) | 1u) & 0x7FF);
		a.offset = base - (loc.xyz << ivec3(BLOCK_DIM_BITS));
	}
}

// Smoke at a point (Flow units); false where no block holds gas.
bool smoke_at(vec3 pos, vec3 cell_inv, inout Accessor a, out float r_smoke) {
	vec3 vidxf = pos * cell_inv;
	ivec4 loc = ivec4(ivec3(floor(vidxf)) >> ivec3(BLOCK_DIM_BITS), p.counts.x);
	accessor_update(a, loc);
	if (!a.valid) {
		r_smoke = 0.0;
		return false;
	}
	r_smoke = textureLod(density_tex, (vidxf + vec3(a.offset)) * DIM_INV, 0.0).a;
	return true;
}

bool intersect_box(vec3 ro, vec3 rd_inv, vec3 bmin, vec3 bmax, out float t0, out float t1) {
	vec3 a = (bmin - ro) * rd_inv;
	vec3 b = (bmax - ro) * rd_inv;
	vec3 lo = min(a, b);
	vec3 hi = max(a, b);
	t0 = max(max(lo.x, lo.y), lo.z);
	t1 = min(min(hi.x, hi.y), hi.z);
	return t1 > max(t0, 0.0);
}

float ign(vec2 px) {
	return fract(52.9829189 * fract(dot(px, vec2(0.06711056, 0.00583715))));
}

void main() {
	ivec2 px = ivec2(gl_GlobalInvocationID.xy);
	if (px.x >= p.size.x || px.y >= p.size.y) {
		return;
	}
	float upm = p.camera.w;
	vec2 uv = (vec2(px) + 0.5) / vec2(p.size.xy);
	float depth = texelFetch(depth_tex, px, 0).r;

	// The view ray, in Flow units, out to the scene (reverse Z: 0 is the far plane).
	vec4 near_h = p.inv_view_projection * vec4(uv * 2.0 - 1.0, 1.0, 1.0);
	vec4 far_h = p.inv_view_projection * vec4(uv * 2.0 - 1.0, max(depth, 1e-7), 1.0);
	vec3 cam = p.camera.xyz * upm;
	vec3 hit = far_h.xyz / far_h.w * upm;
	vec3 dir = hit - near_h.xyz / near_h.w * upm;
	float dir_len = length(dir);
	if (dir_len <= 0.0) {
		return;
	}
	dir /= dir_len;
	float scene_t = depth > 1e-7 ? length(hit - cam) : 1e20;

	vec3 rd_inv = 1.0 / dir;
	float t0;
	float t1;
	if (!intersect_box(cam, rd_inv, p.world_min.xyz, p.world_max.xyz, t0, t1)) {
		return;
	}
	t0 = max(t0, 0.0);
	t1 = min(t1, scene_t);
	if (t1 <= t0) {
		return;
	}

	vec3 block = p.block_size.xyz;
	vec3 cell = block / vec3(p.level[0].xyz + 1u);
	vec3 cell_inv = 1.0 / cell;
	float step_len = min(min(cell.x, cell.y), cell.z) * p.block_size.w;
	float sigma = p.misc.x / upm; // per Flow unit, per unit smoke
	float g = p.misc.y;
	float g2 = g * g;
	int light_count = p.counts.y;
	int shadow_steps = p.counts.z;

	Accessor view_acc;
	view_acc.location = ivec4(0x40000000);
	view_acc.valid = false;
	Accessor light_acc;
	light_acc.location = ivec4(0x40000000);
	light_acc.valid = false;

	float transmittance = 1.0;
	vec3 sum = vec3(0.0);
	float t = t0 + step_len * ign(vec2(px));
	for (int i = 0; i < p.counts.w && t < t1; i++) {
		vec3 pos = cam + dir * t;
		float smoke;
		if (!smoke_at(pos, cell_inv, view_acc, smoke)) {
			// An empty block: jump to where the ray leaves it.
			vec3 bmin = vec3(view_acc.location.xyz) * block;
			float b0;
			float b1;
			intersect_box(cam, rd_inv, bmin, bmin + block, b0, b1);
			t = max(b1, t) + step_len * 0.5;
			continue;
		}
		if (smoke > 0.001) {
			float alpha = 1.0 - exp(-smoke * sigma * step_len);
			vec3 in_light = vec3(0.0);
			vec3 pos_m = pos / upm;
			for (int l = 0; l < light_count; l++) {
				Light L = lights[l];
				vec3 to_light = L.position_range.xyz - pos_m;
				float d = length(to_light);
				float range = L.position_range.w;
				if (d >= range || d <= 0.0) {
					continue;
				}
				vec3 ldir = to_light / d;
				// Godot's own omni falloff (light_functions / scene_forward).
				float nd = d / range;
				nd *= nd;
				nd *= nd;
				nd = max(1.0 - nd, 0.0);
				float atten = nd * nd * pow(max(d, 0.0001), -L.color_decay.w);
				if (L.direction_cutoff.w > -1.5) {
					float cutoff = L.direction_cutoff.w;
					float scos = max(dot(-ldir, L.direction_cutoff.xyz), cutoff);
					float rim = max(0.0001, (1.0 - scos) / (1.0 - cutoff));
					atten *= 1.0 - pow(rim, L.spot.x);
				}
				if (atten <= 0.0) {
					continue;
				}
				// Gas between the point and the light.
				float light_t = 1.0;
				if (shadow_steps > 0) {
					float reach = min(d, p.misc.z) * upm;
					float ds = reach / float(shadow_steps);
					float od = 0.0;
					for (int s = 0; s < shadow_steps; s++) {
						float ls;
						smoke_at(pos + ldir * (ds * (float(s) + 0.5)), cell_inv, light_acc, ls);
						od += ls;
					}
					light_t = exp(-od * sigma * ds);
				}
				// Henyey-Greenstein, scaled so isotropic (g = 0) is 1.
				float c = dot(dir, ldir);
				float phase = (1.0 - g2) / pow(max(1.0 + g2 - 2.0 * g * c, 1e-4), 1.5);
				in_light += L.color_decay.rgb * (atten * phase * light_t);
			}
			sum += transmittance * alpha * in_light;
			transmittance *= 1.0 - alpha;
			if (transmittance < 0.01) {
				break;
			}
		}
		t += step_len;
	}
	if (all(equal(sum, vec3(0.0)))) {
		return;
	}
	vec4 c = imageLoad(color_img, px);
	c.rgb += sum * p.albedo.rgb * p.albedo.w;
	imageStore(color_img, px, c);
}
)";

constexpr uint32_t PARAMS_WORDS = 16 + 4 + 40 + 4 * 7;

} // namespace

bool FlowScatterRD::_ensure(RenderingDevice *p_rd) {
	if (pipeline.is_valid()) {
		return true;
	}
	if (failed) {
		return false;
	}
	failed = true;
	Ref<RDShaderFile> file;
	file.instantiate();
	ERR_FAIL_COND_V_MSG(file->parse_versions_from_text(SCATTER_SHADER) != OK, false, "PhysXFlow3D: light scattering shader failed to compile.");
	Ref<RDShaderSPIRV> spirv = file->get_spirv();
	ERR_FAIL_COND_V(spirv.is_null(), false);
	const String err = spirv->get_stage_compile_error(RD::SHADER_STAGE_COMPUTE);
	ERR_FAIL_COND_V_MSG(!err.is_empty(), false, "PhysXFlow3D: light scattering shader: " + err);
	shader = p_rd->shader_create_from_spirv(file->get_spirv_stages());
	ERR_FAIL_COND_V(!shader.is_valid(), false);
	pipeline = p_rd->compute_pipeline_create(shader);
	ERR_FAIL_COND_V(!pipeline.is_valid(), false);

	RD::SamplerState linear;
	linear.mag_filter = RD::SAMPLER_FILTER_LINEAR;
	linear.min_filter = RD::SAMPLER_FILTER_LINEAR;
	linear.repeat_u = RD::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE;
	linear.repeat_v = RD::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE;
	linear.repeat_w = RD::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE;
	linear_sampler = p_rd->sampler_create(linear);
	RD::SamplerState point;
	point_sampler = p_rd->sampler_create(point);
	params_buffer = p_rd->uniform_buffer_create(PARAMS_WORDS * 4);
	lights_buffer = p_rd->storage_buffer_create(MAX_LIGHTS * sizeof(Light));
	failed = false;
	return true;
}

void FlowScatterRD::run(RenderingDevice *p_rd, const FlowBackend::ScatterSource &p_source, const Params &p_params, const Light *p_lights, int p_light_count, const RID &p_depth, const RID &p_color, const Size2i &p_size) {
	if (!p_source.valid || p_light_count <= 0 || p_size.x <= 0 || p_size.y <= 0 || !_ensure(p_rd)) {
		return;
	}
	const int count = MIN(p_light_count, MAX_LIGHTS);

	uint32_t words[PARAMS_WORDS] = {};
	float *f = reinterpret_cast<float *>(words);
	int32_t *iw = reinterpret_cast<int32_t *>(words);
	uint32_t w = 0;
	for (int c = 0; c < 4; c++) {
		for (int r = 0; r < 4; r++) {
			f[w++] = (float)p_params.inv_view_projection.columns[c][r];
		}
	}
	f[w++] = (float)p_params.camera_position.x;
	f[w++] = (float)p_params.camera_position.y;
	f[w++] = (float)p_params.camera_position.z;
	f[w++] = FlowBackend::UNITS_PER_METER;
	for (int i = 0; i < 40; i++) {
		words[w++] = p_source.level[i];
	}
	f[w++] = p_source.block_size_world[0];
	f[w++] = p_source.block_size_world[1];
	f[w++] = p_source.block_size_world[2];
	f[w++] = MAX(p_params.step_scale, 0.25f);
	iw[w++] = p_source.layer_and_level;
	iw[w++] = count;
	iw[w++] = MAX(p_params.shadow_steps, 0);
	iw[w++] = MAX(p_params.max_steps, 1);
	for (int i = 0; i < 3; i++) {
		f[w++] = p_source.world_min[i];
	}
	w++;
	for (int i = 0; i < 3; i++) {
		f[w++] = p_source.world_max[i];
	}
	w++;
	f[w++] = p_params.color[0];
	f[w++] = p_params.color[1];
	f[w++] = p_params.color[2];
	f[w++] = p_params.strength;
	f[w++] = p_params.sigma;
	f[w++] = CLAMP(p_params.anisotropy, -0.95f, 0.95f);
	f[w++] = MAX(p_params.shadow_reach, 0.01f);
	w++;
	iw[w++] = p_size.x;
	iw[w++] = p_size.y;
	w += 2;
	ERR_FAIL_COND(w != PARAMS_WORDS);
	p_rd->buffer_update(params_buffer, 0, sizeof(words), words);
	p_rd->buffer_update(lights_buffer, 0, count * sizeof(Light), p_lights);

	LocalVector<RD::Uniform> uniforms;
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, Vector<RID>({ linear_sampler, RID::from_uint64(p_source.density_rid) })));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, RID::from_uint64(p_source.table_rid)));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 2, Vector<RID>({ point_sampler, p_depth })));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 3, p_color));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 4, lights_buffer));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 5, params_buffer));
	const RID set = p_rd->uniform_set_create(uniforms, shader, 0);
	ERR_FAIL_COND(!set.is_valid());
	const RD::ComputeListID list = p_rd->compute_list_begin();
	p_rd->compute_list_bind_compute_pipeline(list, pipeline);
	p_rd->compute_list_bind_uniform_set(list, set, 0);
	p_rd->compute_list_dispatch(list, (p_size.x + 7) / 8, (p_size.y + 7) / 8, 1);
	p_rd->compute_list_end();
	p_rd->free_rid(set);
}

void FlowScatterRD::free(RenderingDevice *p_rd) {
	if (p_rd == nullptr) {
		return;
	}
	for (RID *r : { &pipeline, &shader, &linear_sampler, &point_sampler, &params_buffer, &lights_buffer }) {
		if (r->is_valid()) {
			p_rd->free_rid(*r);
			*r = RID();
		}
	}
}
