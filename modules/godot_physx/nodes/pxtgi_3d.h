/**************************************************************************/
/*  pxtgi_3d.h                                                            */
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

#include "physx_gi_effect.h"

#include "core/templates/hash_map.h"
#include "core/templates/local_vector.h"
#include "scene/3d/node_3d.h"
#include "scene/resources/3d/shape_3d.h"
#include "scene/resources/environment.h"

// PXTGI (experimental): the module's own per-pixel global illumination, traced on screen and then against the physics
// shapes, delivered through Forward+'s stock GI path. With SDFGI enabled, every surface shader reads Godot's per-pixel
// GI buffer and multiplies it by its albedo. PhysXGIEffect writes that buffer before the opaque pass; while a visible
// PXTGI3D is in the tree, Godot's own GI step (which would overwrite it) runs a do-nothing shader with the same binding
// layout instead, and SDFGI is kept to its cheapest settings (it is only the switch that makes surfaces read the
// buffer). Hidden or gone, Godot's pipelines and the environment's own SDFGI settings are restored.
class PXTGI3D : public Node3D {
	GDCLASS(PXTGI3D, Node3D);

	static int users;

	float intensity = 1.0f;
	float max_distance = 8.0f;
	float world_ray_distance = 30.0f;
	int debug = 0;
	bool multi_bounce = true;
	int rays_per_texel = 4;
	int reflection_mode = 2;
	float reflection_cutoff = 0.5f;
	ObjectID attached_world;

	// The collision shapes found in the scene (rescanned now and then), each with its body's albedo and emission;
	// and emissive meshes outside any body, which stand in as their bounding box (mesh_proxy).
	struct ShapeEntry {
		ObjectID shape_node;
		Color albedo;
		Color emission;
		bool mesh_proxy = false;
		// Hulls and meshes: their type (4, 5), where their geometry starts in the geometry buffer, their local bounds.
		int geometry_type = 0;
		int geometry_a = 0;
		int geometry_b = 0;
		AABB local_bounds;
		// Where the shape sits on its body and what it is (type, extents), read from its node when last checked: the
		// shapes of a body PhysX moved are placed from the body's own pose, without the scene tree.
		bool posed = false;
		Transform3D in_body;
		float type = 0.0f;
		Vector3 extents;
	};
	// Hull planes, or a mesh's tree and triangles, built once per shape resource.
	struct GeometryBlock {
		int type = 0;
		Vector<float> planes; // hull: normal, offset
		Vector<float> nodes; // mesh: its tree (8 floats a node; indices relative to the mesh)
		Vector<float> triangles; // mesh: 12 floats a triangle (three corners, padded)
		AABB bounds;
	};
	HashMap<ObjectID, GeometryBlock> geometry_blocks;
	Vector<float> geometry;
	uint64_t geometry_version = 0;
	uint32_t geometry_signature = 0;

	const GeometryBlock *_geometry_for(const Ref<Shape3D> &p_shape);
	LocalVector<ShapeEntry> shapes;
	ObjectID sun;
	Ref<Environment> environment; // the one whose SDFGI is the switch, given its own settings back on leaving
	uint32_t last_hash = 0; // what was published last: a change means the GI has to converge again
	LocalVector<ObjectID> omnis;
	LocalVector<ObjectID> spots;
	int rescan_in = 0;
	uint32_t shape_signature = 0; // the shapes found by the last rescan: a change means rebuilding the tree

	// Each shape's GPU record and world bounds (in shape order), kept between frames: only what moved is rewritten,
	// and the tree is refit rather than rebuilt.
	LocalVector<float> entry_floats;
	LocalVector<AABB> entry_bounds;
	LocalVector<uint8_t> entry_valid;
	HashMap<ObjectID, LocalVector<int>> body_entries; // a physics body's shapes, by its node
	LocalVector<int> proxy_entries; // emissive meshes outside any body: checked every frame
	LocalVector<int> emissive_entries; // shapes that give off light: sampled directly
	LocalVector<float> ordered_floats; // the records in the tree's leaf order
	LocalVector<float> tree_nodes;
	LocalVector<int> leaf_order; // leaf slot -> shape
	LocalVector<int> entry_slot; // shape -> leaf slot (-1: not in the tree)
	Vector<float> published_primitives;
	Vector<float> published_nodes;
	int published_count = 0;
	uint64_t primitives_version = 0;
	int verify_in = 0;
	int moves_since_build = 0;

	void _rescan();
	bool _shape_frame(int p_index, Transform3D &r_to_world, float &r_type, Vector3 &r_extents) const;
	void _pack_entry(int p_index, const Transform3D &p_to_world, float p_type, const Vector3 &p_extents, float *r_out, AABB &r_bounds) const;
	bool _write_entry(int p_index, float *r_out, AABB &r_bounds) const;
	bool _refresh_entry(int p_index, float *r_out, AABB &r_bounds);
	bool _entry_frame(int p_index, Transform3D &r_to_world, AABB &r_local) const;
	void _update_primitives(PhysXGIEffect::Snapshot &r_snapshot);

	static void _install();
	static void _uninstall();

public:
	static void free_shared();

private:
	bool active = false; // in the world and visible: delivering the GI
	void _activate();
	void _deactivate();
	void _attach();
	void _detach();
	void _publish();

protected:
	void _notification(int p_what);
	static void _bind_methods();

public:
	PackedStringArray get_configuration_warnings() const override;

	void set_intensity(float p_value) { intensity = MAX(p_value, 0.0f); }
	float get_intensity() const { return intensity; }
	void set_max_distance(float p_value) { max_distance = MAX(p_value, 0.1f); }
	float get_max_distance() const { return max_distance; }
	void set_world_ray_distance(float p_value) { world_ray_distance = MAX(p_value, 0.1f); }
	float get_world_ray_distance() const { return world_ray_distance; }
	void set_rays_per_texel(int p_value) { rays_per_texel = CLAMP(p_value, 1, 4); }
	int get_rays_per_texel() const { return rays_per_texel; }
	void set_multi_bounce(bool p_value) { multi_bounce = p_value; }
	bool get_multi_bounce() const { return multi_bounce; }
	void set_reflection_mode(int p_value) { reflection_mode = CLAMP(p_value, 0, 2); }
	int get_reflection_mode() const { return reflection_mode; }
	void set_reflection_roughness_cutoff(float p_value) { reflection_cutoff = CLAMP(p_value, 0.0f, 1.0f); }
	float get_reflection_roughness_cutoff() const { return reflection_cutoff; }
	void set_debug(int p_value) { debug = p_value; }
	int get_debug() const { return debug; }
};
