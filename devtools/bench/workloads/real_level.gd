extends SceneTree
## Real-level workload of devtools/bench/bench.py (workload id level): the Godot TPS demo
## (godotengine/tps-demo, MIT code, CC-BY 3.0 assets) at the commit pinned in bench.py, exported once
## per series to a .pck by the control editor. The project files are never edited: override.cfg next
## to the .pck and this driver pin what the project leaves to the machine.
##   godot --main-pack <series>/tps-demo.pck --script <this file> --fixed-fps 60 --rendering-driver vulkan
##         -- [--frames=F] [--warmup=W] [--robots=R]
## Frame 0 loads res://level/level.tscn, instances it and spawns R extra robots through the level's own
## spawn_robot() around its four robot spawn points. A Camera3D of its own flies a closed loop through
## the spawn points; its position depends on the frame index only. After W warm-up frames, F frames are
## measured: main_ms is the physics step plus the process step (SceneTree._physics_process / _process,
## which run first, to a marker node with the highest priority, which runs last); render_cpu_ms is the
## viewport's measured render CPU time plus the frame setup CPU time; gpu_ms is printed, never judged.
## Prints one BENCH line; BENCH_FAIL: <reason> on failure.

const LEVEL := "res://level/level.tscn"
const SEED := 20261121
const RING_RADIUS := 2.0  # extra robots stand on a ring around each spawn point
const CAMERA_HEIGHT := 2.0
const QUIT_MS := 500

var _frames := 600
var _warmup := 120
var _robots := 32
var _frame := -1
var _quit_usec := 0  # set once the BENCH line is printed
var _vp: RID
var _camera: Camera3D
var _path := PackedVector3Array()
var _added_usec := 0
var _start_usec := 0
var _phys_start_usec := 0
var _phys_ms := 0.0
var _load_ms := 0.0
var _instance_ms := 0.0
var _first_frame_ms := 0.0
var _main_ms: Array[float] = []
var _render_ms: Array[float] = []
var _gpu_ms: Array[float] = []

# Allocation counts (bench builds only: the fork/alloc_count monitor exists).
var _counted := false
var _read_cost: Array[int] = [0, 0]
var _load_allocs := 0
var _fly_before: Array[int] = []


# Processed last in both steps: its time closes the frame's main-thread window.
class EndMarker:
	extends Node
	var driver: Object

	func _process(_delta: float) -> void:
		driver.end_process()

	func _physics_process(_delta: float) -> void:
		driver.end_physics()


func _initialize() -> void:
	for arg in OS.get_cmdline_user_args():
		if arg.begins_with("--frames="):
			_frames = arg.get_slice("=", 1).to_int()
		elif arg.begins_with("--warmup="):
			_warmup = arg.get_slice("=", 1).to_int()
		elif arg.begins_with("--robots="):
			_robots = arg.get_slice("=", 1).to_int()
	_counted = Performance.has_custom_monitor(&"fork/alloc_count")
	if _counted:
		var first := _read_counts()
		var second := _read_counts()
		_read_cost = [second[0] - first[0], second[1] - first[1]]


func _physics_process(_delta: float) -> bool:
	_phys_start_usec = Time.get_ticks_usec()
	return false


func end_physics() -> void:
	_phys_ms = (Time.get_ticks_usec() - _phys_start_usec) / 1000.0


func _in_window(p_frame: int) -> bool:
	return p_frame > _warmup and p_frame <= _warmup + _frames


func end_process() -> void:
	if _in_window(_frame):
		_main_ms.append((Time.get_ticks_usec() - _start_usec) / 1000.0 + _phys_ms)


func _process(_delta: float) -> bool:
	_start_usec = Time.get_ticks_usec()
	_frame += 1
	if _quit_usec > 0:
		return _start_usec >= _quit_usec
	if _frame == 0:
		return not _load_level()
	if _frame == 1:
		_first_frame_ms = (_start_usec - _added_usec) / 1000.0
	if _in_window(_frame - 1):  # the render times read now are the previous frame's
		_render_ms.append(RenderingServer.viewport_get_measured_render_time_cpu(_vp) + RenderingServer.get_frame_setup_time_cpu())
		_gpu_ms.append(RenderingServer.viewport_get_measured_render_time_gpu(_vp))
	if _counted and _frame == _warmup + 1:
		_fly_before = _read_counts()
	if _frame == _warmup + _frames + 1:
		_finish()
		return false
	_move_camera()
	return false


func _load_level() -> bool:
	# The level's _ready applies the Settings autoload (user://settings.ini, if a player saved one):
	# every option is pinned to the demo's defaults, then windowed, no vsync, no fps cap.
	var settings: Node = root.get_node_or_null(^"Settings")
	if settings == null:
		printerr("BENCH_FAIL: no Settings autoload (is the .pck the TPS demo?)")
		quit(1)
		return false
	for section in settings.DEFAULTS:
		for key in settings.DEFAULTS[section]:
			settings.config_file.set_value(section, key, settings.DEFAULTS[section][key])
	settings.config_file.set_value("video", "display_mode", Window.MODE_WINDOWED)
	settings.config_file.set_value("video", "vsync", DisplayServer.VSYNC_DISABLED)
	settings.config_file.set_value("video", "max_fps", 0)
	settings.config_file.set_value("rendering", "gi_type", settings.GIType.VOXEL_GI)
	settings.config_file.set_value("rendering", "gi_quality", settings.GIQuality.LOW)

	var before: Array = _read_counts() if _counted else [0, 0]
	var t0 := Time.get_ticks_usec()
	var packed: PackedScene = load(LEVEL)
	var t1 := Time.get_ticks_usec()
	if packed == null:
		printerr("BENCH_FAIL: cannot load ", LEVEL)
		quit(1)
		return false
	var level: Node3D = packed.instantiate()
	root.add_child(level)  # runs the level's _ready: GI setup, 4 robots, the player
	seed(SEED)  # level.gd and the forklifts call randomize() in _ready
	var robot_points: Array[Node] = level.get_node(^"RobotSpawnpoints").get_children()
	var holder := Node3D.new()  # same (identity) parent space as RobotSpawnpoints
	holder.name = "BenchSpawnpoints"
	level.add_child(holder)
	var per_point := ceili(_robots / float(robot_points.size()))
	for i in _robots:
		var point: Marker3D = robot_points[i % robot_points.size()]
		var angle := TAU * floori(i / float(robot_points.size())) / per_point
		var marker := Marker3D.new()
		marker.transform = point.transform.translated(Vector3(cos(angle), 0.0, sin(angle)) * RING_RADIUS)
		holder.add_child(marker)
		level.spawn_robot(marker)
	var t2 := Time.get_ticks_usec()
	if _counted:
		var after := _read_counts()
		_load_allocs = after[0] - before[0] - _read_cost[0]
	_load_ms = (t1 - t0) / 1000.0
	_instance_ms = (t2 - t1) / 1000.0
	_added_usec = t2
	_pin_random_choices(level)

	# The player stands still (no input, mouse left alone); the bench camera replaces its camera.
	var player: Node = level.get_node_or_null(^"SpawnedNodes/1")
	if player == null:
		printerr("BENCH_FAIL: the level spawned no player")
		quit(1)
		return false
	player.transform = level.get_node(^"PlayerSpawnpoints").get_child(0).transform
	var input: Node = player.get_node(^"InputSynchronizer")
	input.set_process(false)
	input.set_process_input(false)
	input.set_physics_process(false)
	Input.set_mouse_mode(Input.MOUSE_MODE_VISIBLE)
	root.set_disable_input(true)

	for point in robot_points:
		_path.append(point.position + Vector3.UP * CAMERA_HEIGHT)
	_path.append(level.get_node(^"PlayerSpawnpoints").get_child(0).position + Vector3.UP * CAMERA_HEIGHT)
	_camera = Camera3D.new()
	root.add_child(_camera)
	_move_camera()

	var marker := EndMarker.new()
	marker.driver = self
	marker.process_priority = 1 << 30
	marker.process_physics_priority = 1 << 30
	root.add_child(marker)
	_vp = root.get_viewport_rid()
	RenderingServer.viewport_set_measure_render_time(_vp, true)
	return true


# What randomize() chose in _ready: the player's spawn point (placed above) and each flying forklift's model.
func _pin_random_choices(p_level: Node) -> void:
	for node in p_level.find_children("*", "Node3D", true, false):
		var script: Script = node.get_script()
		if script != null and script.resource_path.ends_with("flying_forklift.gd"):
			var models := node.get_child(0).get_children()
			for i in models.size():
				models[i].visible = i == 0


# A closed loop through the waypoints, one lap over the warm-up and measured frames.
func _move_camera() -> void:
	var t := float(_frame) / float(_warmup + _frames + 1) * _path.size()
	var i := int(t) % _path.size()
	var from := _path[i]
	var to := _path[(i + 1) % _path.size()]
	_camera.position = from.lerp(to, t - floorf(t))
	if not _camera.position.is_equal_approx(to):
		_camera.look_at(to)
	_camera.make_current()


func _finish() -> void:
	var counts := ""
	if _counted:
		var after := _read_counts()
		counts = " load_allocs=%d fly_allocs=%d fly_reallocs=%d" % [
			_load_allocs, after[0] - _fly_before[0] - _read_cost[0], after[1] - _fly_before[1] - _read_cost[1]]
	print("BENCH: workload=level robots=%d frames=%d load_ms=%.3f instance_ms=%.3f first_frame_ms=%.3f main_ms_p50=%.4f main_ms_p95=%.4f render_cpu_ms_p50=%.4f render_cpu_ms_p95=%.4f gpu_ms_p50=%.4f%s" % [
		_robots, _frames, _load_ms, _instance_ms, _first_frame_ms, _pct(_main_ms, 0.5), _pct(_main_ms, 0.95),
		_pct(_render_ms, 0.5), _pct(_render_ms, 0.95), _pct(_gpu_ms, 0.5), counts])
	# Freed before the tree ends, then QUIT_MS ms of empty frames: a freed audio player's playback is
	# removed by the audio thread (after a fade-out) and then the main thread, and one still listed at exit
	# leaks the level music ("resources still in use"). Robots' timers and the laser blasts (added to the
	# root) go with it.
	for child in root.get_children():
		if child.name != &"Settings":
			child.free()
	_quit_usec = Time.get_ticks_usec() + QUIT_MS * 1000


# Both totals, read back to back; two calls in a row give the cost of one read pair.
func _read_counts() -> Array[int]:
	return [Performance.get_custom_monitor(&"fork/alloc_count"), Performance.get_custom_monitor(&"fork/realloc_count")]


func _pct(p_values: Array[float], p_p: float) -> float:
	var sorted := p_values.duplicate()
	sorted.sort()
	return sorted[clampi(int(p_p * (sorted.size() - 1)), 0, sorted.size() - 1)]
