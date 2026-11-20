extends SceneTree
## godot-benchmarks subset workload of devtools/bench/bench.py (workload id gdbench).
##   godot --headless --path misc/scene_bench_project --script <this file> -- --files=<abs dir> [--extra=<abs file>]...
## Loads every .gd in --files and each --extra file, in file name order, calls each benchmark_*
## method (in name order) once to warm up and once timed, and prints one line per method:
##   BENCH: workload=gdbench case=<file stem>.<method> time_ms=... allocs=... reallocs=...
## A file that does not load or has no benchmark_ methods prints BENCH_FAIL: <reason> and exits
## with code 1. A method that raises a script error prints SCRIPT ERROR, which fails the sample.

var _counted := false


func _initialize() -> void:
	var paths: Array[String] = []
	for arg in OS.get_cmdline_user_args():
		if arg.begins_with("--files="):
			var dir := arg.trim_prefix("--files=")
			for file in DirAccess.get_files_at(dir):
				if file.get_extension() == "gd":
					paths.append(dir.path_join(file))
		elif arg.begins_with("--extra="):
			paths.append(arg.trim_prefix("--extra="))
	if paths.is_empty():
		_fail("no benchmark files (--files=<dir>, --extra=<file>)")
		return
	paths.sort_custom(func(a: String, b: String) -> bool: return a.get_file() < b.get_file())
	_counted = Performance.has_custom_monitor(&"fork/alloc_count")
	for path in paths:
		var script: Script = load(path)
		if script == null or not script.can_instantiate():
			_fail("%s does not load" % path.get_file())
			return
		var methods: Array[String] = []
		for method in script.get_script_method_list():
			if method["name"].begins_with("benchmark_"):
				methods.append(method["name"])
		if methods.is_empty():
			_fail("%s has no benchmark_ methods" % path.get_file())
			return
		methods.sort()
		var bench: Object = script.new()
		for method in methods:
			seed(12345)  # alloc.gd scatters nodes with randi(): the same order every run
			bench.call(method)  # warm-up
			seed(12345)
			var first := _read_counts()
			var before := _read_counts()
			var start := Time.get_ticks_usec()
			bench.call(method)
			var time_ms := (Time.get_ticks_usec() - start) / 1000.0
			var after := _read_counts()
			var counts := ""
			if _counted:
				# The second read pair minus the first is the cost of one read pair.
				counts = " allocs=%d reallocs=%d" % [after[0] - 2 * before[0] + first[0], after[1] - 2 * before[1] + first[1]]
			print("BENCH: workload=gdbench case=%s.%s time_ms=%.4f%s" % [path.get_file().get_basename(), method, time_ms, counts])
		bench = null
	quit()


func _fail(p_reason: String) -> void:
	printerr("BENCH_FAIL: ", p_reason)
	quit(1)


func _read_counts() -> Array[int]:
	if not _counted:
		return [0, 0]
	return [Performance.get_custom_monitor(&"fork/alloc_count"), Performance.get_custom_monitor(&"fork/realloc_count")]
