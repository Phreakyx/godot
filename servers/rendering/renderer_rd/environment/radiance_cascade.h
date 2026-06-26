/**************************************************************************/
/*  radiance_cascade.h                                                    */
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

#pragma once

// Radiance Cascades global illumination (Alexander Sannikov's technique).
//
// A compute-only, real-time GI solver that runs alongside SDFGI / VoxelGI as a
// sibling indirect-light source. Like them it produces a screen-space diffuse
// irradiance target the forward pass multiplies by material albedo (RB_TEX_AMBIENT,
// see GI::process_gi()); unlike them it needs no bake step.
//
// THE TWO HALVES
//   1) SCENE REPRESENTATION (the trace backend) — what the probe cones sample.
//      Backend 0 (default) is a camera-centred voxel grid built by this class:
//      geometry rasterized into a 3D grid (albedo / normal / emission / injected
//      radiance), with anisotropic mips and a jump-flood SDF that make it cheap to
//      cone-trace. Coarse clipmap levels extend range. The backend is switchable
//      (see TraceBackend) so the same cascades can instead trace SDFGI's SDF, or a
//      hardware ray-tracing pipeline, for evaluation.
//   2) RADIANCE CASCADES — a hierarchy of probe sets. Cascade 0 is dense with few
//      directions and short rays; each coarser cascade is sparser with more angular
//      resolution and longer ray intervals. Probes cone-trace the scene, then the
//      cascades merge far->near so the cheap near field inherits the far field's
//      angular detail. Probes live in a per-cascade dense pool addressed through a
//      transient world-hashed map, allocated only where the current view needs them.
//
// PER-FRAME PIPELINE (see RadianceCascade::process()):
//   update voxels -> voxelize dynamic occluders -> clear/rebuild/allocate probes ->
//   trace probes vs the scene -> merge cascades -> gather c0 into a half-res
//   irradiance buffer -> a-trous denoise -> bilateral upsample to full-res ->
//   write RB_TEX_AMBIENT.
//
// OWNERSHIP — this mirrors GI::SDFGI: per-viewport GPU state lives in this
// RenderBufferCustomDataRD (created per render buffer, RB_SCOPE_RC); the compute
// shaders/pipelines are shared and owned by GI (RadianceCascadeShaders). Camera,
// lights and geometry come from the engine's render data, never the SceneTree.

#include "core/math/aabb.h"
#include "core/math/projection.h"
#include "core/math/transform_3d.h"
#include "core/templates/local_vector.h"
#include "servers/rendering/renderer_rd/shaders/environment/rc3d_voxel_emission_mip.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/environment/rc3d_voxel_mip_aniso.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/environment/rc3d_voxel_sdf.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/environment/rc_clip_inject.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/environment/rc_composite.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/environment/rc_dyn_occ_temporal.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/environment/rc_irradiance_atrous.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/environment/rc_irradiance_upsample.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/environment/rc_patch_add.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/environment/rc_patch_clear.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/environment/rc_patch_gather.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/environment/rc_patch_indirect.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/environment/rc_patch_lookup.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/environment/rc_patch_merge.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/environment/rc_patch_neighbours.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/environment/rc_patch_rebuild.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/environment/rc_patch_reduce.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/environment/rc_patch_trace.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/environment/rc_slab_clear.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/environment/rc_voxel_debug.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/environment/rc_voxel_inject.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/environment/rc_voxel_unpack.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/environment/rc_voxelize_dynamic.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/environment/rc_voxelize_mesh.glsl.gen.h"
#include "servers/rendering/renderer_rd/storage_rd/render_buffer_custom_data_rd.h"
#include "servers/rendering/rendering_device.h"

#define RB_SCOPE_RC SNAME("rc")

class RenderDataRD;
class RenderSceneBuffersRD;

namespace RendererRD {

class GI;

// One record per compute pass; each mirrors the push_constant block in the matching
// rc_*.glsl. Layouts are std430-tight and 16-byte aligned, so field order and the
// padN padding must stay in lockstep with the shaders.

// rc_patch_clear.glsl — wipe the transient hashmap (buckets->EMPTY) + reset per-cascade live counters.
struct RCPatchClearPushConstant {
	uint32_t total_buckets;
	uint32_t num_cascades;
	uint32_t pad[2];
};

// rc_patch_rebuild.glsl — per dense id: evict aged ids (free-list push) else re-insert (key->id).
struct RCPatchRebuildPushConstant {
	uint32_t frame;
	uint32_t evict_age;
	uint32_t cascade;
	uint32_t pad;
};

// rc_patch_add.glsl — find-or-allocate probes for a screen-pixel grid over cascades [begin,end).
struct RCPatchAddPushConstant {
	uint32_t screen_width;
	uint32_t screen_height;
	uint32_t cascade_begin;
	uint32_t cascade_end;
	float z_near;
	float z_far;
	uint32_t frame; // stamps last_seen (persistence + live dedup)
	uint32_t pad;
};

// rc_patch_indirect.glsl — turn live probe counts into indirect-dispatch args.
struct RCPatchIndirectPushConstant {
	uint32_t num_cascades;
	uint32_t local_size;
	uint32_t pad[2];
};

// rc_patch_trace.glsl — which cascade this dispatch traces.
struct RCPatchTracePushConstant {
	uint32_t cascade;
	uint32_t local_transmittance;
	uint32_t frame;
	uint32_t amortize_n;
};

// rc_patch_lookup.glsl — debug readout of probes / radiance for one cascade.
struct RCPatchLookupPushConstant {
	uint32_t screen_width;
	uint32_t screen_height;
	uint32_t debug_kind;
	uint32_t cascade;
	float z_near;
	float z_far;
	float pad0[2];
	float sky_color[3];
	float pad1;
};

// rc_patch_gather.glsl — sample cascade-0 probes into the half-res irradiance buffer.
struct RCPatchGatherPushConstant {
	uint32_t screen_width;
	uint32_t screen_height;
	uint32_t pad0[2];
	float z_near;
	float z_far;
	float pad1[2];
	float sky_color[3]; // sky fallback where no probe is hit
	float pad2;
};

// rc_patch_merge.glsl — merge cascade c+1 down into cascade c.
struct RCPatchMergePushConstant {
	uint32_t cascade;
	uint32_t frame;
	uint32_t amortize_n;
	uint32_t pad;
};

// rc3d_voxel_emission_mip.glsl — downsample one emission mip level.
struct RCVoxelMipPushConstant {
	uint32_t dst_res;
	uint32_t pad[3];
};

// rc3d_voxel_mip_aniso.glsl — build 6-axis directional radiance mips.
struct RCMipAnisoPushConstant {
	uint32_t dst_res;
	uint32_t src_is_aniso;
	uint32_t pad[2];
};

// rc_irradiance_upsample.glsl — bilateral half->full upsample (depth/normal aware).
struct RCUpsamplePushConstant {
	uint32_t full_w;
	uint32_t full_h;
	uint32_t half_w;
	uint32_t half_h;
	float z_near;
	float z_far;
	float sigma_z;
	float normal_pow;
	float sky_color[3];
	float gi_intensity;
};

// rc_irradiance_atrous.glsl — edge-aware a-trous denoise (step = hole size, doubles per pass).
struct RCAtrousPushConstant {
	uint32_t half_w;
	uint32_t half_h;
	int32_t step;
	uint32_t pad;
	float z_near;
	float z_far;
	float sigma_z;
	float normal_pow;
};

// rc_voxelize_dynamic.glsl — rasterize dynamic proxy tris into the occupancy grid.
struct RCDynVoxelizePushConstant {
	float vox_origin[3];
	uint32_t res;
	float vox_extent[3];
	uint32_t tri_count;
};

// rc_dyn_occ_temporal.glsl — temporally accumulate the dynamic occupancy grid.
struct RCDynOccTemporalPushConstant {
	uint32_t res;
	float decay;
	uint32_t pad[2];
};

// rc_voxelize_mesh.glsl — rasterize static tris into a grid slab.
struct RCVoxelizePushConstant {
	float vox_extent[3];
	uint32_t res;
	int32_t slab_lo[3];
	uint32_t tri_count;
	int32_t slab_dim[3];
	uint32_t pad;
};

// rc_slab_clear.glsl — clear a toroidal shell before re-voxelizing it.
struct RCSlabClearPushConstant {
	int32_t slab_lo[3];
	uint32_t res;
	int32_t slab_dim[3];
	uint32_t pad;
};

// rc_voxel_inject.glsl — inject direct light into a level-0 grid slab.
// blend_alpha < 1 cross-fades a relight over several frames instead of popping.
struct RCInjectSlabPushConstant {
	float vox_origin[3];
	uint32_t light_count;
	float voxel_size;
	float blend_alpha;
	uint32_t pad0[2];
	int32_t slab_lo[3];
	uint32_t res;
	int32_t slab_dim[3];
	uint32_t pad1;
	int32_t phase[3];
	uint32_t pad2;
};

// rc_clip_inject.glsl — inject sun (+ lights) into a coarse clipmap level.
struct RCClipInjectSlabPushConstant {
	float sun_dir[3];
	float blend_alpha;
	float sun_color[3];
	float voxel_size;
	int32_t slab_lo[3];
	uint32_t res;
	int32_t slab_dim[3];
	uint32_t light_count;
	int32_t phase[3];
	uint32_t pad;
};

// rc3d_voxel_sdf.glsl — jump-flood distance field (mode = init/flood/finalize, step = jump size).
struct RCSdfPushConstant {
	uint32_t mode;
	int32_t step;
	uint32_t res;
	uint32_t pad0;
	int32_t phase[3];
	uint32_t pad1;
};

// rc_voxel_unpack.glsl — unpack the packed voxelization targets into the RC grid.
struct RCVoxelUnpackPushConstant {
	int32_t phase[3];
	uint32_t res;
	int32_t slab_lo[3];
	uint32_t pad0;
	int32_t slab_dim[3];
	uint32_t pad1;
};

// rc_voxel_debug.glsl — raymarch a grid level to the screen for visualization.
struct RCVoxelDebugPushConstant {
	uint32_t sw;
	uint32_t sh;
	uint32_t res;
	uint32_t max_steps;
	float vox_origin[3];
	float voxel_size;
	float vox_extent[3];
	float occ_threshold;
};

// rc_composite.glsl — final blend of GI over the lit scene color.
// TEMPORARY: a bring-up output path. Replaced by writing RB_TEX_AMBIENT (deleted then).
struct RCCompositePushConstant {
	uint32_t screen_width;
	uint32_t screen_height;
	float gi_intensity;
	uint32_t debug_mode;
};

// rc_patch_trace set 2 — level-0 grid placement (world origin/size) + cone step cap.
struct RCTraceParams {
	float vox_origin[3];
	float voxel_size;
	float vox_extent[3];
	uint32_t max_steps;
};

// Camera UBO — both directions of the transform so probe passes can go
// screen->world (inverse) and world->screen (forward) for reprojection.
struct RCCameraData {
	float inv_proj[16];
	float inv_view[16];
	float fwd_proj[16];
	float fwd_view[16];
	float jitter[2];
	float pad[2];
};

// One light, SSBO record read by the inject passes (4 x vec4 = 64 B, std430).
struct RCLightData {
	float position[3]; // omni/spot world pos
	float inv_range; // 1/range (0 = directional)
	float direction[3]; // light->scene dir
	float type; // 0 dir, 1 omni, 2 spot
	float color[3]; // linear color x energy
	float spot_cos_in; // spot inner-cone cos
	float spot_cos_out; // spot outer-cone cos
	float pad[3];
};

// One cascade's geometry + buffer offsets. Uploaded once to the cascade SSBO (bound
// at b7) and shared by every patch shader; the GLSL mirror must match (48 B std430).
struct RCCascadeDesc {
	float spacing; // probe spacing (world units)
	float t_start; // ray interval start
	float t_end; // ray interval end
	float aperture; // cone aperture
	uint32_t dirs; // == oct_res^2
	uint32_t oct_res;
	uint32_t bucket_off; // transient hashmap region [bucket_off, +bucket_cap)
	uint32_t bucket_cap;
	uint32_t probe_off; // dense id-space offset
	uint32_t probe_cap;
	uint32_t rad_off; // dense radiance offset
	uint32_t pad;
};

// Shared, viewport-independent compute resources for Radiance Cascades. Owned by GI
// (mirrors SDFGIShader): created once, used by every per-viewport RadianceCascade.
struct RadianceCascadeShaders {
	// One {shader object, version, pipeline} triple per compute pass.
	Rc3DVoxelEmissionMipShaderRD voxel_emission_mip;
	Rc3DVoxelMipAnisoShaderRD voxel_mip_aniso;
	Rc3DVoxelSdfShaderRD voxel_sdf;
	RcClipInjectShaderRD clip_inject;
	RcCompositeShaderRD composite; // TEMPORARY (bring-up output)
	RcDynOccTemporalShaderRD dyn_occ_temporal;
	RcIrradianceAtrousShaderRD irradiance_atrous;
	RcIrradianceUpsampleShaderRD irradiance_upsample;
	RcPatchAddShaderRD patch_add;
	RcPatchClearShaderRD patch_clear;
	RcPatchGatherShaderRD patch_gather;
	RcPatchIndirectShaderRD patch_indirect;
	RcPatchLookupShaderRD patch_lookup;
	RcPatchMergeShaderRD patch_merge;
	RcPatchNeighboursShaderRD patch_neighbours;
	RcPatchRebuildShaderRD patch_rebuild;
	RcPatchReduceShaderRD patch_reduce;
	RcPatchTraceShaderRD patch_trace;
	RcSlabClearShaderRD slab_clear;
	RcVoxelDebugShaderRD voxel_debug;
	RcVoxelInjectShaderRD voxel_inject;
	RcVoxelUnpackShaderRD voxel_unpack;
	RcVoxelizeDynamicShaderRD voxelize_dynamic;
	RcVoxelizeMeshShaderRD voxelize_mesh;

	RID voxel_emission_mip_shader, voxel_emission_mip_pipeline;
	RID voxel_mip_aniso_shader, voxel_mip_aniso_pipeline;
	RID voxel_sdf_shader, voxel_sdf_pipeline;
	RID clip_inject_shader, clip_inject_pipeline;
	RID composite_shader, composite_pipeline;
	RID dyn_occ_temporal_shader, dyn_occ_temporal_pipeline;
	RID irradiance_atrous_shader, irradiance_atrous_pipeline;
	RID irradiance_upsample_shader, irradiance_upsample_pipeline;
	RID patch_add_shader, patch_add_pipeline;
	RID patch_clear_shader, patch_clear_pipeline;
	RID patch_gather_shader, patch_gather_pipeline;
	RID patch_indirect_shader, patch_indirect_pipeline;
	RID patch_lookup_shader, patch_lookup_pipeline;
	RID patch_merge_shader, patch_merge_pipeline;
	RID patch_neighbours_shader, patch_neighbours_pipeline;
	RID patch_rebuild_shader, patch_rebuild_pipeline;
	RID patch_reduce_shader, patch_reduce_pipeline;
	RID patch_trace_shader, patch_trace_pipeline;
	RID slab_clear_shader, slab_clear_pipeline;
	RID voxel_debug_shader, voxel_debug_pipeline;
	RID voxel_inject_shader, voxel_inject_pipeline;
	RID voxel_unpack_shader, voxel_unpack_pipeline;
	RID voxelize_dynamic_shader, voxelize_dynamic_pipeline;
	RID voxelize_mesh_shader, voxelize_mesh_pipeline;

	void init();
	void free();
};

class RadianceCascade : public RenderBufferCustomDataRD {
	GDCLASS(RadianceCascade, RenderBufferCustomDataRD)

public:
	enum {
		MAX_CASCADES = 5, // probe hierarchy depth (4-6 reasonable)
		MAX_CLIP = 5, // voxel levels: 0 = fine grid, 1..4 = coarse clipmap rings
		MAX_LIGHTS = 256,
		CLIP_ANISO_M = 4, // level-0 aniso depth -> L0 snap = 16 vox = 4 m
		MAX_DYN_TRIS = 8192, // dynamic-occluder proxy triangle cap
		MAX_STATIC_TRIS = 1 << 22, // static voxelize triangle buffer cap
		FLOATS_PER_TRI = 20, // 5 * vec4: a, b, c, emission, albedo
	};

	// The scene representation the probe cones sample. Switchable so the cascades can
	// be evaluated against alternative backends without changing the cascade math.
	// Selected via a ShaderRD variant on rc_patch_trace (set 2 = backend interface).
	enum TraceBackend {
		TRACE_BACKEND_VOXEL, // RC's own voxel grid (default, standalone) -- shipped first
		TRACE_BACKEND_SDFGI, // trace the engine's SDFGI cascaded SDF (requires SDFGI enabled)
		TRACE_BACKEND_HWRT, // Vulkan hardware ray tracing (future)
	};

	// Which intermediate buffer process() visualizes.
	enum DebugView {
		DEBUG_OFF,
		DEBUG_PATCHES,
		DEBUG_PATCHES_RADIANCE,
		DEBUG_PROBE_TRACE,
		DEBUG_GATHER,
		DEBUG_VOXEL,
	};

	// Back-pointer to the owning GI (holds the shared RadianceCascadeShaders).
	GI *gi = nullptr;

	virtual void configure(RenderSceneBuffersRD *p_render_buffers) override;
	virtual void free_data() override;
	~RadianceCascade();

	// Lifecycle (mirrors GI::SDFGI::create/update). create() allocates the per-viewport
	// GPU resources for a given screen size; process() runs the whole GI chain for one
	// frame and writes the diffuse irradiance target; both are driven from GI.
	void create(GI *p_gi, const Size2i &p_size);
	void process(RenderDataRD *p_render_data, RID p_depth, RID p_normal_roughness, RID p_color, RID p_ambient);

	// Indirect-light intensity (Environment > Radiance Cascades > rc_energy); folded
	// into the upsample/composite push constants.
	void set_gi_intensity(float p_intensity) { gi_intensity = p_intensity; }

	// Spread a full directional refresh over N frames (Environment > rc_amortization);
	// 1 = refresh every frame. Forced to 1 internally while a relight cross-fade is armed.
	void set_trace_amortization(int p_frames) { trace_amortization = (uint32_t)CLAMP(p_frames, 1, 64); }

	// Debug visualization (Environment > rc_debug): 0 = off. When active, the renderer
	// calls dispatch_debug() post-tonemap and blits get_debug_texture() over the frame.
	void set_debug_view(int p_view) { debug_view = p_view; }
	bool is_debug_view_active() const { return debug_view != 0; }
	RID get_debug_texture() const { return debug_tex; }
	void dispatch_debug();

	// Geometry voxelization (toroidal streaming): scroll_to() snaps the grid to follow the
	// camera in whole-voxel steps and records the thin shell(s) that scrolled into view this
	// frame (the whole grid on the first frame or a teleport). The renderer then rasterizes
	// each shell region into the render targets, and process() unpacks + injects just those
	// cells -- so movement cost is a thin shell, not a full re-bake. See [[rc-movement-streaming]].
	void scroll_to(const Vector3 &p_cam_origin);
	int voxel_shell_count() const { return (int)pending_shells.size(); }
	AABB voxel_shell_bounds(int p_i) const; // world AABB of shell p_i (for the ortho cameras)
	Vector3i voxel_shell_offset(int p_i) const { return pending_shells[p_i].lo; } // render-grid offset
	Vector3i voxel_shell_size(int p_i) const { return pending_shells[p_i].dim; }
	int voxel_resolution() const { return vox_res; }
	RID get_render_albedo() const { return render_albedo; }
	RID get_render_emission() const { return render_emission; }
	RID get_render_emission_aniso() const { return render_emission_aniso; }
	RID get_render_geom_facing() const { return render_geom_facing; }

private:
	void free_resources();

	// ── Cascade table + static resources ──
	void build_cascade_table();
	void build_static_sets(); // set-0/2 uniform sets, built once by create()
	void update_trace_params(); // refresh the trace-backend UBOs (level-0 + clipmap placement)

	// ── Inputs (engine-native; NOT a SceneTree walk -- see header note) ──
	// TODO(port): drive these from render data instead of the GDExtension's Node3D path.
	void update_camera(const Projection &p_projection, const Transform3D &p_transform);
	void rebuild_per_frame_sets(); // set-1 depth/normal/color sets from this frame's buffers
	void update_lights(RenderDataRD *p_render_data); // from the renderer light instance list
	void update_geometry(RenderDataRD *p_render_data); // feed the voxelizer from render geometry instances

	// ── Per-pass dispatch (order lives in process()) ──
	uint32_t effective_amortization() const; // N=1 while a relight cross-fade is active
	void dispatch_patch_clear();
	void dispatch_patch_rebuild();
	void dispatch_patch_add();
	void dispatch_patch_trace();
	void dispatch_patch_neighbours();
	void dispatch_patch_merge();
	void dispatch_patch_gather();
	void dispatch_patch_lookup(uint32_t p_debug_kind);
	void dispatch_voxel_unpack(const Vector3i &p_lo, const Vector3i &p_dim); // packed render targets -> voxel grid (region)
	void dispatch_inject(const Vector3i &p_lo, const Vector3i &p_dim); // direct light from light_buffer -> voxel_tex radiance (region)
	void dispatch_voxel_mips();
	void dispatch_emission_mips();
	void dispatch_voxel_debug();
	void dispatch_dynamic_voxelize();
	void dispatch_dyn_occ_temporal();
	void dispatch_irradiance_atrous();
	void dispatch_irradiance_upsample();
	void dispatch_composite(); // TEMPORARY bring-up output; replaced by RB_TEX_AMBIENT write

	// ── Voxel scene / SDF ──
	void build_sdf(); // one-shot jump flood (whole field this frame)
	void sdf_amortize_begin(); // arm the amortized flood (sdf_pass = 0)
	bool sdf_amortize_step(); // advance ~2 flood passes/frame; returns true the frame it completes

	// ════════════════════════════ MEMBER DATA ════════════════════════════

	// ── Core ──
	RenderingDevice *rd = nullptr;
	Size2i screen_size;
	Size2i half_size; // gather / denoise / upsample run at half-res
	TraceBackend trace_backend = TRACE_BACKEND_VOXEL;
	uint32_t frame_index = 0;

	// ── Camera ──
	RID camera_ubo; // RCCameraData (inverse + forward view/proj)
	float z_near = 0.05;
	float z_far = 4000.0;

	// ── Cascade table ──
	RCCascadeDesc cascades[MAX_CASCADES];
	uint32_t total_buckets = 0;
	uint32_t total_probes = 0;
	uint32_t total_rad = 0;
	RID cascade_buffer; // SSBO of the table, bound at b7
	float dist_mult = 1.0;
	float step_mult = 1.0;
	float cascade_scale = 2.0;
	float interval_overlap = 0.1;
	bool local_transmittance = true;
	uint32_t trace_amortization = 1; // full directional refresh spread over N frames (1 = off)
	int probe_seed_max_h = 1080; // coarse-cascade probe seed-lattice height

	// ── Probe store (dense pool + transient hashmap) ──
	RID patch_buckets; // transient hashmap (hash -> dense id)
	RID patch_alloc; // per-cascade live counters
	RID patch_keys;
	RID patch_world;
	RID patch_live; // compact live-id list
	RID patch_freelist; // recycled dense ids
	RID patch_alloc_state; // (free_top, next_id) per cascade
	RID probe_radiance; // packed rgba uint per (probe, dir)
	RID probe_rad_tag; // per-id owner hash, persisted (temporal amortization)
	RID probe_last_seen; // per-id last-seen frame (0 = free)
	RID patch_neighbours; // 8 precomputed c+1 dense ids per probe (merge cache)
	RID patch_indirect_buffer; // indirect-dispatch args for trace/merge
	RID reduced_radiance; // angular pre-reduce scratch (merge ratio > 1)
	RID probe_inspect_buffer; // debug readback
	uint32_t evict_age = 60;

	// ── Patch per-frame uniform sets (depth + normal) ──
	RID patch_add_set0, patch_add_set1;
	RID patch_clear_set0;
	RID patch_rebuild_set0;
	RID patch_lookup_set0, patch_lookup_set1;
	RID patch_indirect_set0;
	RID patch_trace_set0;
	RID patch_gather_set0;
	RID patch_merge_set0;
	RID patch_neighbours_set0;
	RID patch_reduce_set0;

	// ── Voxel grid (level 0) ──
	RID voxel_tex; // radiance grid (the trace target)
	RID voxel_sampler;
	RID voxel_linear_sampler;
	RID voxel_albedo; // rgba8
	RID voxel_normal; // rgba8
	RID voxel_emission; // rgba16f, full mips
	int vox_res = 256;
	Vector3 vox_origin = Vector3(-32, -2, -32);
	Vector3 vox_extent = Vector3(64, 64, 64);
	Vector3i vox_phase; // origin_voxel % res (toroidal addressing)
	bool voxel_dirty = true; // force a full re-voxelize next scroll_to (first frame / teleport)

	// Shells that scrolled into view this frame: render-grid offset + size in voxels. The
	// renderer voxelizes each, process() unpacks + injects each, then process() clears them.
	struct VoxelShell {
		Vector3i lo;
		Vector3i dim;
	};
	LocalVector<VoxelShell> pending_shells;

	// ── Geometry voxelization render targets (SDFGI-style PASS_MODE_SDF output) ──
	// The renderer rasterizes scene instances into these packed integer grids; an
	// unpack pass then fills voxel_albedo/normal/emission. Replaces the CPU voxelizer.
	RID render_albedo; // R16_UINT, packed RGB
	RID render_emission; // R32_UINT, packed
	RID render_emission_aniso; // R32_UINT, packed
	RID render_geom_facing; // R32_UINT, packed face mask -> normal

	// ── Voxel mips (anisotropic + emission) ──
	int vox_mip_levels = 1;
	int aniso_levels = 0;
	RID voxel_aniso[6]; // rgba16f, res/2^3
	LocalVector<RID> aniso_views[6];
	LocalVector<RID> emission_mip_views;

	// ── Voxelize (static + dynamic tris -> grid) ──
	RID tri_buffer; // GPU scratch the voxelize pass reads
	uint32_t tri_count = 0;
	RID voxelize_set0;
	RID slab_clear_set;
	RID dyn_occ; // r8 dynamic occupancy
	RID dyn_occ_acc; // temporally accumulated occupancy
	RID dyn_tri_buffer;
	uint32_t dyn_tri_count = 0;
	RID dyn_voxelize_set0;
	RID dyn_occ_temporal_set0;
	float dyn_occ_decay = 0.95;

	// ── Trace backend (descriptor set 2 = the scene-representation interface) ──
	RID trace_params_ubo;
	RID trace_voxel_set2;
	int trace_max_steps = 64;
	RID voxel_debug_set0;
	RID voxel_debug_clip_set[MAX_CLIP];

	// ── Inject (direct light -> voxels) ──
	RID inject_set0;
	RID voxel_unpack_set0;

	// ── SDF march ──
	RID sdf_tex; // r16f distance field
	RID sdf_seed_a, sdf_seed_b; // ping-pong flood
	RID sdf_set_write_a, sdf_set_write_b;
	int sdf_pass = -1; // -1 idle; else amortized flood step
	bool sdf_seed_in_a = true;
	bool sdf_built_once = false; // first bake floods synchronously; re-bakes amortize

	// ── Voxel clipmap (coarse levels) ──
	bool clip_origins_inited = false;
	bool level_dirty[MAX_CLIP] = { true, true, true, true, true };
	int clip_levels = 5;
	RID clip_grid[MAX_CLIP]; // [0] unused (level 0 = voxel_tex); [1..4] coarse
	RID clip_voxelize_set[MAX_CLIP];
	RID clip_inject_set[MAX_CLIP];
	RID clip_slab_clear_set[MAX_CLIP];
	Vector3 clip_origin[MAX_CLIP];
	RID clip_params_ubo;
	RID dummy_clip_tex; // 1^3 black, fills unused bindings
	RID clip_albedo, clip_normal, clip_emission; // shared coarse-voxelize scratch

	// ── Lights ──
	RID light_buffer; // MAX_LIGHTS * RCLightData
	uint32_t light_count = 0;
	LocalVector<RCLightData> lights;

	// ── Relight (fade + dynamic tracking) ──
	int relight_frames = 0;
	int relight_frames_max = 10;
	int relight_track_frames = 0;
	float relight_track_alpha = 0.35;
	int relight_track_settle = 8;
	int clip_relight_frames = 0;

	// ── Sun / sky ──
	Vector3 sun_dir = Vector3(-0.3, -0.8, -0.5).normalized();
	Vector3 sun_color = Vector3(1.0, 0.95, 0.85) * 3.0;
	Vector3 sky_color = Vector3(0.1, 0.13, 0.18);

	// ── Irradiance: half-res gather -> a-trous -> upsample ──
	RID irradiance_tex; // full-res output (-> RB_TEX_AMBIENT)
	RID irradiance_half; // half-res gather target
	RID irradiance_half_b; // a-trous ping-pong scratch
	RID atrous_set0_h2s, atrous_set0_s2h;
	RID atrous_set1; // per-frame depth + normal
	int atrous_passes = 4;
	RID upsample_set0;
	RID upsample_set1; // per-frame depth + normal
	RID upsample_set2; // c0 probes (direct full-res gather on edges)
	RID point_sampler;
	float sigma_z = 0.05;
	float normal_pow = 8.0;

	// ── Composite (TEMPORARY bring-up output) ──
	RID composite_set0;
	RID composite_set1; // per-frame scene color (sampler + image)
	RID dummy_albedo_tex; // 1x1 white placeholder
	RID albedo_sampler;
	RID color_sampler;
	RID dummy_color_tex;
	float gi_intensity = 1.0;

	// ── Shared scene-input samplers ──
	RID depth_sampler;
	RID normal_sampler;

	// ── Per-frame engine inputs (set by process() from the render buffers) ──
	RID frame_depth;
	RID frame_normal;
	RID frame_color;
	RID frame_ambient; // RB_TEX_AMBIENT: the upsample writes the GI here (forward shader reads it)

	// ── Debug ──
	RID debug_tex;
	int debug_view = 0;
	uint32_t debug_cascade = 0;
	int debug_clip_level = 0;
};

} // namespace RendererRD
