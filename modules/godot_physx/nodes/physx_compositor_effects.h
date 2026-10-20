/**************************************************************************/
/*  physx_compositor_effects.h                                            */
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

#include "core/templates/hash_map.h"
#include "core/templates/hash_set.h"
#include "core/templates/local_vector.h"
#include "scene/resources/compositor.h"

class Camera3D;
class World3D;

// Puts the module's own compositor effects (the screen-space fluid, the Flow
// ray marcher) into a world's frame and keeps them in a fixed order, whoever
// installs first: a Compositor on the World3D when the world has none, or added
// at the RenderingServer level to the user's own (their resource is never
// changed or saved). Each effect injecting on its own would overwrite the
// other's injection, and their order would follow whichever node came first.
class PhysXCompositorEffects {
public:
	// Lower runs first. The fluid goes before Flow: it leaves Flow a depth with
	// its surface in it, so gas in front of the liquid draws over it and gas
	// behind it is hidden.
	enum Order {
		ORDER_FLUID = 0,
		ORDER_FLOW = 1,
	};

	// Every frame (cheap when nothing changed): the world's compositor, and the
	// camera's own if it has one (it replaces the world's for that camera).
	static void install(World3D *p_world, Camera3D *p_camera, const Ref<CompositorEffect> &p_effect, int p_order);
	// Once the world has no node left that needs p_effect.
	static void uninstall(const ObjectID &p_world, const Ref<CompositorEffect> &p_effect);

private:
	struct Placed {
		int order = 0;
		RID effect;
	};
	struct CompositorState {
		LocalVector<RID> seen; // the user's own effects when we last pushed
		LocalVector<Placed> ours; // sorted by order
	};
	struct WorldState {
		Ref<Compositor> created; // ours, set on the world while it has none
		HashSet<RID> effects;
		HashSet<ObjectID> compositors; // every compositor we injected into
	};
	static HashMap<ObjectID, CompositorState> compositors;
	static HashMap<ObjectID, WorldState> worlds;

	static void _place(const Ref<Compositor> &p_compositor, const Ref<CompositorEffect> &p_effect, int p_order);
	static void _withdraw(const ObjectID &p_compositor, const RID &p_effect);
	static void _push(Compositor *p_compositor, const LocalVector<RID> &p_own, const CompositorState &p_state);
};
