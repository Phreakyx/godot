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
// Installs itself like PhysXFlowRenderEffect: a Compositor on the World3D
// when the world has none, or added at the RenderingServer level to the
// user's own (their resource is never changed or saved).
class PhysXFluidRenderEffect : public CompositorEffect {
	GDCLASS(PhysXFluidRenderEffect, CompositorEffect);

public:
	// What one fluid hands the effect each step (main thread -> render thread).
	struct Snapshot {
		PackedFloat32Array points; // per particle: world x, y, z, splat radius
		Color absorption = Color(0.45, 0.12, 0.06); // per meter of fluid, per channel
		Color sky = Color(0.55, 0.68, 0.8); // what the surface reflects
		float refraction = 0.04f; // screen-space offset per meter of thickness
		float smoothing = 0.04f; // world radius of the depth smoothing
		float depth_threshold = 0.04f; // a depth jump larger than this is an edge the smoothing keeps
		float fade_thickness = 0.06f; // fluid thinner than this fades into the scene
		Vector3 light_direction = Vector3(0, -1, 0); // world, toward the scene
		Color light_color = Color(1, 1, 1);
		Vector3 center;
	};

	static void set_fluid(const ObjectID &p_fluid, const Snapshot &p_snapshot);
	static void remove_fluid(const ObjectID &p_fluid);

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

	struct WorldState {
		int fluids = 0;
		Ref<Compositor> created;
		HashMap<ObjectID, LocalVector<RID>> injected;
	};
	static HashMap<ObjectID, WorldState> worlds;
	static void _inject(WorldState &r_state, const Ref<Compositor> &p_compositor);
	static void _restore(const ObjectID &p_compositor);

	// GPU state (render thread only).
	static constexpr uint32_t PARAMS_FLOATS = 72; // the shaders' Params block, std140
	bool failed = false;
	RID shaders[6];
	RID pipelines[6];
	RID linear_sampler, point_sampler;
	RID params_buffer;
	RID points_buffer;
	uint32_t points_capacity = 0;
	Size2i size;
	RID depth_u, thick_u, smooth_a, smooth_b, scene_copy;

	bool _ensure(RenderingDevice *p_rd);
	void _ensure_targets(RenderingDevice *p_rd, const Size2i &p_size);
	void _free_targets(RenderingDevice *p_rd);
	void _dispatch(RenderingDevice *p_rd, int p_pass, const Vector<RD::Uniform> &p_uniforms, uint32_t p_x, uint32_t p_y, const void *p_push, uint32_t p_push_size);
	void _render(int p_callback_type, const RenderData *p_render_data);

protected:
	static void _bind_methods() {}
};
