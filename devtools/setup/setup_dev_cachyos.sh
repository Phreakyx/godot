#!/usr/bin/env bash
# Sets up a CachyOS (Arch) PC for developing this Godot build, in one go. The
# Linux counterpart of setup_dev_windows.ps1. Safe to run again: every step
# that is already done is skipped.
#
#   1. Checks the tools: Git, Python 3, SCons, GCC, Clang/LLD/LLVM, CMake and
#      Godot's Arch build packages. With --install-deps it installs the
#      missing ones with pacman (the only step that uses sudo).
#   2. Sets the git remotes the runbook (CUSTOM_BUILD.md) uses: upstream,
#      nvidia, physx and diginaat (watched, never pushed: their push URL is
#      DISABLED). origin stays as cloned.
#   3. Builds the PhysX (CPU), Blast and Flow SDKs once, in a physx-sdk folder
#      next to this repository. PhysX GPU dynamics are Windows-only.
#   4. Writes custom.py (not tracked) with the SDK paths, keeping any other
#      lines already in it, so a build is just:
#        scons platform=linuxbsd target=editor production=yes
#   5. With --build, builds the GCC and Clang editors, their unit-test builds,
#      and a GCC dev_build test build (the only one with DEV_ASSERT).
#
# Usage, from anywhere in the repository:
#   devtools/setup/setup_dev_cachyos.sh
#   devtools/setup/setup_dev_cachyos.sh --install-deps --build
#   ... --physx-dir /path/to/physx-sdk   build the SDKs somewhere else
#
# Keep the clone on a Linux file system: builds on an NTFS mount are slow and
# can break on permissions and case.
set -euo pipefail

install_deps=0
build=0
physx_dir=""
while [ $# -gt 0 ]; do
	case "$1" in
		--install-deps) install_deps=1 ;;
		--build) build=1 ;;
		--physx-dir)
			[ $# -ge 2 ] || { echo "--physx-dir needs a folder" >&2; exit 2; }
			physx_dir="$2"
			shift
			;;
		-h | --help)
			sed -n '2,25p' "$0"
			exit 0
			;;
		*) echo "Unknown option: $1 (see --help)" >&2; exit 2 ;;
	esac
	shift
done

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$root"
[ -n "$physx_dir" ] || physx_dir="$(dirname "$root")/physx-sdk"
physx_dir="$(realpath -m "$physx_dir")"

step() { printf '\n== %s\n' "$1"; }
ok() { printf '   OK  %s\n' "$1"; }
info() { printf '   ..  %s\n' "$1"; }
fail() { printf '   !!  %s\n' "$1" >&2; exit 1; }

# 1. Tools.
step "Checking the tools"
ok "Repository path: $root"
fs="$(stat -f -c %T . 2>/dev/null || echo unknown)"
case "$fs" in
	fuseblk | ntfs* | ntfs3) info "WARNING: the repository is on an NTFS mount ($fs). Clone it onto the Linux file system for reliable, fast builds." ;;
esac

packages=(git python scons pkgconf gcc clang lld llvm cmake ninja make
	libxcursor libxinerama libxi libxrandr wayland-utils mesa glu libglvnd alsa-lib libpulse)
missing=()
for p in "${packages[@]}"; do
	# -T also counts a package another one provides (pacman -Q does not).
	pacman -T "$p" > /dev/null 2>&1 || missing+=("$p")
done
if [ ${#missing[@]} -gt 0 ]; then
	if [ "$install_deps" -eq 1 ]; then
		info "Installing: ${missing[*]}"
		sudo pacman -S --needed --noconfirm "${missing[@]}"
	else
		info "Missing packages: ${missing[*]}"
		info "Install them with:"
		info "  sudo pacman -S --needed ${missing[*]}"
		fail "or run this script again with --install-deps."
	fi
fi
ok "Packages: ${packages[*]}"
pyver="$(python3 -c 'import sys; print("%d.%d" % sys.version_info[:2])')"
python3 -c 'import sys; sys.exit(sys.version_info < (3, 8))' || fail "Python $pyver is too old; Godot needs 3.8 or newer."
ok "Python $pyver"
ok "$(git --version)"
ok "$(gcc --version | head -n 1)"
ok "$(clang --version | head -n 1)"
ok "SCons $(scons --version | sed -n 's/.*SCons: v\([^ ,]*\).*/\1/p' | head -n 1)"

# 2. Remotes (names as in CUSTOM_BUILD.md). origin is the clone's own remote.
step "Setting the git remotes"
declare -A want=(
	[upstream]="https://github.com/godotengine/godot.git"
	[nvidia]="https://github.com/NVIDIA-RTX/godot.git"
	[physx]="https://github.com/uno1982/godot.git"
	[diginaat]="https://github.com/Diginaat/godot.git"
)
if url="$(git remote get-url origin 2> /dev/null)"; then
	ok "origin -> $url"
else
	info "No origin remote; this clone pushes nowhere until you add one."
fi
for name in upstream nvidia physx diginaat; do
	if git remote get-url "$name" > /dev/null 2>&1; then
		ok "$name -> $(git remote get-url "$name")"
	else
		git remote add "$name" "${want[$name]}"
		ok "$name -> ${want[$name]} (added)"
	fi
	# Source remotes are fetched, never pushed.
	git remote set-url --push "$name" DISABLED
done

# 3. PhysX (CPU), Blast and Flow SDKs. Folder names as build_physx.py writes them
# for linuxbsd.
step "PhysX, Blast and Flow SDKs"
arch="$(uname -m)"
physx_sdk="$physx_dir/physx/install/linux64-godot/PhysX"
blast_sdk="$physx_dir/blast/_build/linux-$arch/release/blast-sdk"
flow_sdk="$physx_dir/flow"
if [ -d "$physx_sdk" ] && [ -d "$blast_sdk/include" ] && [ -f "$flow_sdk/_build/linux-$arch/release/libnvflow.so" ]; then
	ok "Already built in $physx_dir"
else
	info "Building them in $physx_dir (the first time takes a while)"
	python3 modules/godot_physx/misc/build_physx.py --blast --flow --src "$physx_dir" || fail "build_physx.py failed; see its output above."
	for path in "$physx_sdk" "$blast_sdk"; do
		[ -d "$path" ] || fail "Expected $path after the SDK build."
	done
	ok "Built in $physx_dir"
fi

# 4. custom.py: the build options of this PC (not tracked by git).
step "Writing custom.py"
custom="# Build options of this PC, written by devtools/setup/setup_dev_cachyos.sh.
# SCons reads this file on every build; it is not tracked by git.
physx_sdk = r\"$physx_sdk\"
physx_gpu = \"no\"
blast_sdk = r\"$blast_sdk\"
flow_sdk = r\"$flow_sdk\""
# Keep the lines this script doesn't write (other options, comments), so a rerun
# never drops them.
if [ -f custom.py ]; then
	extra="$(grep -vE '^(physx_sdk|physx_gpu|blast_sdk|flow_sdk)[[:space:]]*=|^# (Build options of this PC|SCons reads this file)' custom.py || true)"
	[ -z "$extra" ] || custom="$custom
$extra"
fi
if [ -f custom.py ] && [ "$(cat custom.py)" = "$custom" ]; then
	ok "custom.py is up to date"
else
	printf '%s\n' "$custom" > custom.py
	ok "custom.py: PhysX (CPU only), Blast, Flow"
fi

# 5. Optional builds. production=yes turns on LTO (full for GCC, thin for
# Clang); Clang's ThinLTO links with lld.
built=()
run_scons() {
	info "scons $*"
	scons -j"$(nproc)" platform=linuxbsd "$@" || fail "Build failed: scons $*"
}
if [ "$build" -eq 1 ]; then
	step "Building the GCC editor"
	run_scons target=editor production=yes
	built+=(bin/godot.linuxbsd.editor.x86_64)
	step "Building the Clang editor"
	run_scons target=editor production=yes use_llvm=yes linker=lld
	built+=(bin/godot.linuxbsd.editor.x86_64.llvm)
	step "Building the GCC and Clang unit-test builds"
	run_scons target=editor production=yes tests=yes object_prefix=t_ extra_suffix=tests
	built+=(bin/godot.linuxbsd.editor.x86_64.tests)
	run_scons target=editor production=yes use_llvm=yes linker=lld tests=yes object_prefix=t_ extra_suffix=tests
	built+=(bin/godot.linuxbsd.editor.x86_64.llvm.tests)
	step "Building the GCC dev_build test build (DEV_ASSERT compiled in)"
	run_scons target=editor dev_build=yes tests=yes
	built+=(bin/godot.linuxbsd.editor.dev.x86_64)
	for b in "${built[@]}"; do
		if [ -f "$b" ]; then ok "$b"; else info "Not found under the expected name: $b (see ls bin/)"; fi
	done
fi

step "Done"
echo "   Build the editor:   scons platform=linuxbsd target=editor production=yes [use_llvm=yes linker=lld]"
echo "   Unit tests:         bin/godot.linuxbsd.editor.x86_64.tests --test"
echo "   Smoke test:         devtools/test/smoke_linux.sh"
echo "   Measurement series: hold the compilers so control and candidate use the same one. Add this"
echo "                       line to /etc/pacman.conf yourself (this script never edits it):"
echo "                         IgnorePkg = gcc gcc-libs clang llvm llvm-libs lld"
echo "   Maintenance:        CUSTOM_BUILD.md"
