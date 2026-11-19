#!/usr/bin/env bash
# The Linux smoke test: the editor and a game start on Vulkan and PhysX (CPU)
# initializes. Same pass rule as smoke.ps1: exit code 0, no ERROR lines
# (except the screen reader driver one), and the expected log lines.
#
#   devtools/test/smoke_linux.sh                       # every bin/godot.linuxbsd.editor.x86_64* (no tests builds)
#   devtools/test/smoke_linux.sh bin/godot.linuxbsd.editor.x86_64.llvm
#   ... --physx-example /path/to/godot-physx-example
#
# Uses Wild Ox Studios' example project
# (git clone https://github.com/uno1982/godot-physx-example next to this
# repository). Logs go to devtools/out/smoke/linux.
set -uo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
example="$(dirname "$root")/godot-physx-example"
binaries=()
while [ $# -gt 0 ]; do
	case "$1" in
		--physx-example)
			[ $# -ge 2 ] || { echo "--physx-example needs a folder" >&2; exit 2; }
			example="$2"
			shift
			;;
		-h | --help)
			sed -n '2,12p' "$0"
			exit 0
			;;
		*) binaries+=("$1") ;;
	esac
	shift
done
if [ ${#binaries[@]} -eq 0 ]; then
	for b in "$root"/bin/godot.linuxbsd.editor.x86_64*; do
		[ -x "$b" ] && [[ "$(basename "$b")" != *tests* ]] && binaries+=("$b")
	done
fi
if [ ${#binaries[@]} -eq 0 ]; then
	echo "No editor binary in bin/. Build one first: devtools/setup/setup_dev_cachyos.sh --build" >&2
	exit 1
fi
logs="$root/devtools/out/smoke/linux"
mkdir -p "$logs"
failed=0

result() { # result PASS|FAIL name detail
	printf '%s  %-34s %s\n' "$1" "$2" "$3"
	[ "$1" = PASS ] || failed=$((failed + 1))
}

if [ ! -f "$example/project.godot" ]; then
	result FAIL "PhysX example project" "missing $example; git clone https://github.com/uno1982/godot-physx-example next to this repository"
	echo "1 test(s) failed." >&2
	exit 1
fi

run_test() { # run_test binary name extra-args...
	local bin="$1" name="$2"
	shift 2
	local log
	log="$logs/$(basename "$bin")_$(echo "$name" | sed 's/[^[:alnum:]]\+/_/g').log"
	"$bin" --rendering-driver vulkan --verbose --path "$example" --quit-after 600 "$@" > "$log" 2>&1
	local code=$?
	local errors physx
	errors="$(grep -E '^ERROR' "$log" | grep -v 'screen reader support driver')"
	physx="$(grep -m 1 -E 'PhysX 5.*initialized' "$log")"
	if [ "$code" -ne 0 ]; then
		result FAIL "$name" "exit code $code; log $log"
	elif [ -n "$errors" ]; then
		result FAIL "$name" "$(echo "$errors" | wc -l) errors, first: $(echo "$errors" | head -n 1)"
	elif [ -z "$physx" ]; then
		result FAIL "$name" "missing 'PhysX ... initialized'"
	elif ! grep -qE '^Vulkan [0-9.]+ - .* - Using Device' "$log"; then
		result FAIL "$name" "missing the Vulkan device line"
	else
		result PASS "$name" "$(echo "$physx" | sed 's/^[[:space:]]*//')"
	fi
}

for bin in "${binaries[@]}"; do
	echo "Editor: $bin ($("$bin" --version 2> /dev/null | head -n 1))"
	run_test "$bin" "PhysX, game"
	run_test "$bin" "PhysX, editor" --editor
done

if [ "$failed" -gt 0 ]; then
	echo "$failed test(s) failed; logs in $logs" >&2
	exit 1
fi
echo "All Linux smoke tests passed."
