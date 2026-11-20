extends SceneTree
## GDScript workloads of devtools/bench/bench.py (workload ids script:<mode>): the three modes of
## devtools/profile/core_workload.gd, run for a fixed number of frames instead of a time, so the
## operation and allocation counts are the same for fast and slow builds.
##   godot --headless --path misc/scene_bench_project --fixed-fps 60 --script <this file> -- --mode=<nodes|script|instance> [--frames=F] [--warmup=W]
## nodes: 10,000 nodes, 200 freed and re-added per frame, a group call. script: 2,000 Agent objects
## with Dictionary stats, % formatting and String joins. instance: a 2,000-node PackedScene
## instanced and freed per frame. Prints one BENCH line; BENCH_FAIL: <reason> on failure.

const MODES := ["nodes", "script", "instance"]

var _mode := "nodes"
var _frames := 120
var _warmup := 30
var _frame := 0
var _last_usec := 0
var _frame_ms: Array[float] = []
var _nodes: Array[Node3D] = []
var _mesh: Mesh
var _scene: PackedScene
var _logic: Array = []

# Allocation counts (bench builds only: the fork/alloc_count monitor exists).
var _counted := false
var _counts_before: Array[int] = []
var _read_cost: Array[int] = []


class Agent:
	extends RefCounted
	var stats := {"health": 100, "speed": 3.5, "name": "agent"}
	var position := Vector3.ZERO
	var velocity := Vector3.ZERO
	var path: Array[Vector3] = []
	var tags: Array[String] = []

	func step(p_delta: float, p_target: Vector3, p_frame: int) -> String:
		var to := p_target - position
		if to.length_squared() > 0.01:
			velocity = velocity.lerp(to.normalized() * float(stats["speed"]), 0.1)
		position += velocity * p_delta
		path.append(position)
		if path.size() > 16:
			path.pop_front()
		stats["health"] = int(stats["health"]) - (1 if p_frame % 7 == 0 else 0)
		if stats["health"] <= 0:
			stats["health"] = 100
		var key := "k%d" % (p_frame % 32)
		stats[key] = stats.get(key, 0) + 1
		if tags.size() < 8:
			tags.append(str(p_frame))
		return "%s:%d" % [stats["name"], stats["health"]]


func _initialize() -> void:
	for arg in OS.get_cmdline_user_args():
		if arg.begins_with("--mode="):
			_mode = arg.get_slice("=", 1)
		elif arg.begins_with("--frames="):
			_frames = arg.get_slice("=", 1).to_int()
		elif arg.begins_with("--warmup="):
			_warmup = arg.get_slice("=", 1).to_int()
	if not MODES.has(_mode):
		printerr("BENCH_FAIL: unknown mode ", _mode)
		quit(1)
		return
	Engine.max_fps = 0
	if DisplayServer.get_name() != "headless":
		DisplayServer.window_set_vsync_mode(DisplayServer.VSYNC_DISABLED)
		var camera := Camera3D.new()
		camera.position = Vector3(0, 40, 120)
		root.add_child(camera)
	_counted = Performance.has_custom_monitor(&"fork/alloc_count")
	_mesh = BoxMesh.new()
	match _mode:
		"nodes":
			for i in 10000:
				_nodes.append(_add_node(i))
		"script":
			for i in 2000:
				var agent := Agent.new()
				agent.position = Vector3(i % 50, 0, i / 50)
				_logic.append(agent)
		"instance":
			var template := Node3D.new()
			for i in 2000:
				var child := MeshInstance3D.new()
				child.mesh = _mesh
				child.name = "Part%d" % i
				child.position = Vector3(i % 40, 0, i / 40)
				template.add_child(child)
				child.owner = template
			_scene = PackedScene.new()
			_scene.pack(template)
			template.free()


func _add_node(p_index: int) -> Node3D:
	var node := MeshInstance3D.new()
	node.mesh = _mesh
	node.position = Vector3(p_index % 100, 0, p_index / 100)
	node.add_to_group("movers" if p_index % 2 == 0 else "others")
	root.add_child(node)
	return node


func _step() -> void:
	var t := _frame * 0.016
	match _mode:
		"nodes":
			for i in range(_frame % 5, _nodes.size(), 5):
				var node := _nodes[i]
				node.position.y = sin(t + i * 0.01)
				node.rotate_y(0.01)
			for i in 200:
				var index := (_frame * 200 + i) % _nodes.size()
				_nodes[index].queue_free()
				_nodes[index] = _add_node(index)
			call_group("others", "set_visible", _frame % 2 == 0)
		"script":
			var target := Vector3(sin(t) * 20.0, 0, cos(t) * 20.0)
			var log := PackedStringArray()
			for agent in _logic:
				log.append((agent as Agent).step(0.016, target, _frame))
			var joined := ",".join(log.slice(0, 64))
			if joined.length() == 0:
				printerr("BENCH_FAIL: empty join")
		"instance":
			var instance := _scene.instantiate()
			root.add_child(instance)
			instance.queue_free()


func _process(_delta: float) -> bool:
	var now := Time.get_ticks_usec()
	if _frame > 0 and _frame > _warmup:
		_frame_ms.append((now - _last_usec) / 1000.0)
	_last_usec = now
	if _counted and _frame == _warmup:
		var first := _read_counts()
		_counts_before = _read_counts()
		_read_cost = [_counts_before[0] - first[0], _counts_before[1] - first[1]]
	_frame += 1
	_step()
	if _frame_ms.size() >= _frames:
		var counts := ""
		if _counted:
			var after := _read_counts()
			counts = " allocs=%d reallocs=%d" % [after[0] - _counts_before[0] - _read_cost[0], after[1] - _counts_before[1] - _read_cost[1]]
		print("BENCH: workload=script mode=%s frames=%d frame_ms_p50=%.4f frame_ms_p95=%.4f%s" % [
			_mode, _frames, _pct(_frame_ms, 0.5), _pct(_frame_ms, 0.95), counts])
		quit()
		return true
	return false


# Both totals, read back to back; two calls in a row give the cost of one read pair.
func _read_counts() -> Array[int]:
	return [Performance.get_custom_monitor(&"fork/alloc_count"), Performance.get_custom_monitor(&"fork/realloc_count")]


func _pct(p_values: Array[float], p_p: float) -> float:
	var sorted := p_values.duplicate()
	sorted.sort()
	return sorted[clampi(int(p_p * (sorted.size() - 1)), 0, sorted.size() - 1)]
