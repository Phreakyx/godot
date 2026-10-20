/**************************************************************************/
/*  physx_fluid_render_effect.h                                           */
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

#pragma once

#include "core/math/color.h"
#include "core/os/mutex.h"
#include "core/templates/hash_map.h"
#include "core/templates/local_vector.h"
#include "scene/resources/compositor.h"
#include "servers/rendering/rendering_device.h"

class Camera3D;
class World3D;
class RenderData;

// Screen-space surface for PhysXParticleFluid3D (screen_space on): instead of
// marching an isosurface mesh every step, the particles are splatted as
// spheres into a depth + thickness buffer each frame, the depth is smoothed,
// and the surface is shaded straight into the frame (refraction, absorption,
// fresnel, a sun highlight). Cost follows the pixels the fluid covers, not its
// volume, and nothing is read back.
//
// PBD foam is drawn here too, as soft white splats over the surface (fading
// with how far below it they are). The composite also leaves a copy of the
// scene depth with the fluid's surface in it, which PhysXFlowRenderEffect
// marches against (it runs after this one -- see PhysXCompositorEffects).
class PhysXFluidRenderEffect : public CompositorEffect {
	GDCLASS(PhysXFluidRenderEffect, CompositorEffect);

public:
	// What one fluid hands the effect each step (main thread -> render thread).
	struct Snapshot {
		PackedFloat32Array points; // per particle: world x, y, z, splat radius
		PackedFloat32Array foam; // per foam particle: world x, y, z, splat radius
		Color foam_color = Color(1, 1, 1, 0.85); // a: the most a foam patch covers
		Color absorption = Color(0.45, 0.12, 0.06); // per meter of fluid, per channel
		Color sky = Color(0.55, 0.68, 0.8); // what the surface reflects
		float refraction = 0.04f; // screen-space offset per meter of thickness
		float smoothing = 0.04f; // world radius of the depth smoothing
		float depth_threshold = 0.04f; // a depth jump larger than this is an edge the smoothing keeps
		float fade_thickness = 0.06f; // fluid thinner than this fades into the scene
		int resolution_divisor = 2; // frame pixels per surface texel, each way (1, 2 or 4)
		Color surface = Color(0, 0, 0, 0); // body colour (linear); a: opacity per meter of fluid, 0 = clear
		float highlight_power = 160.0f; // the sun highlight's tightness (Blinn-Phong exponent)
		float highlight_strength = 1.0f;
		Color emission = Color(0, 0, 0);
		Vector3 light_direction = Vector3(0, -1, 0); // world, toward the scene
		Color light_color = Color(1, 1, 1);
		Vector3 center;
	};

	static void set_fluid(const ObjectID &p_fluid, const Snapshot &p_snapshot);
	static void set_foam(const ObjectID &p_fluid, const PackedFloat32Array &p_foam);
	static void remove_fluid(const ObjectID &p_fluid);

	// The scene depth with the fluid's surface in it, from this frame's
	// composite for those render buffers, or an invalid RID when no fluid drew
	// there this frame (or hasn't yet: the user put Flow's effect ahead of it).
	// Render thread; each one is handed out once.
	static RID take_merged_depth(const ObjectID &p_buffers);

	static void attach_world(World3D *p_world);
	static void detach_world(const ObjectID &p_world);
	static void install(World3D *p_world, Camera3D *p_camera);
	static Ref<PhysXFluidRenderEffect> get_shared();
	static void free_shared();

	PhysXFluidRenderEffect();
	~PhysXFluidRenderEffect();

private:
	static Mutex mutex;
	static HashMap<ObjectID, Snapshot> fluids;

	static HashMap<ObjectID, int> worlds; // fluids in screen-space mode, per world
	struct MergedDepth {
		RID texture;
		uint64_t frame = 0; // RenderingDevice::get_frames_drawn() when drawn
	};
	static HashMap<ObjectID, MergedDepth> merged_depths; // per render buffers

	// GPU state (render thread only).
	static constexpr uint32_t PARAMS_FLOATS = 96; // the shaders' Params block, std140
	bool failed = false;
	RID shaders[7];
	RID pipelines[7];
	RID linear_sampler, point_sampler;
	RID params_buffer;
	RID points_buffer;
	uint32_t points_capacity = 0;
	RID foam_buffer;
	uint32_t foam_capacity = 0;
	Size2i size; // the surface buffers
	Size2i frame_size; // the colour copy
	RID depth_u, thick_u, foam_depth_u, foam_u, smooth_a, smooth_b, scene_copy, merged_depth;

	bool _ensure(RenderingDevice *p_rd);
	void _ensure_targets(RenderingDevice *p_rd, const Size2i &p_size, const Size2i &p_frame);
	void _free_targets(RenderingDevice *p_rd);
	void _dispatch(RenderingDevice *p_rd, int p_pass, const Vector<RD::Uniform> &p_uniforms, uint32_t p_x, uint32_t p_y, const void *p_push, uint32_t p_push_size);
	void _render(int p_callback_type, const RenderData *p_render_data);

protected:
	static void _bind_methods() {}
};
