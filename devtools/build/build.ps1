# Builds this fork with the options of this PC (custom.py, written by
# devtools\setup\setup_dev_windows.ps1), with production=yes (unless -Dev) so the
# builds share their objects and stay incremental.
#
#   devtools\build\build.ps1                    # editor
#   devtools\build\build.ps1 -Mono              # C# (.NET) editor, glue and assemblies
#   devtools\build\build.ps1 -Target templates  # release and debug export templates
#   devtools\build\build.ps1 -Target templates -Mono
#   devtools\build\build.ps1 -Target release    # everything a GitHub release needs
#   devtools\build\build.ps1 -Target template_release   # only the release template
#   ... -Jobs 10      parallel jobs (default: all cores)
#   ... -KeepGoing    scons -k: report all compile errors in one pass
#   ... -Tests        unit-test build (tests=yes, own objects), run with --test:
#                     bin\godot.windows.editor.x86_64.tests.console.exe
#   ... -Variant cand -Defines "FORK_BACKEND_X=1"
#                     A/B candidate from the same source (SCons cppdefines plus
#                     extra_suffix): own objects, binary ...x86_64.cand[.tests].console.exe
#                     next to the control. -Defines takes several, space-separated.
#   ... -Dev          dev_build=yes instead of production=yes (DEV_ASSERT compiled in):
#                     bin\godot.windows.editor.dev.x86_64[.tests].console.exe
#   ... -Bench        benchmark build (devtools/bench/bench.py): debug_symbols=yes,
#                     disable_path_overrides=no (template_release then takes --path and
#                     --script) and cppdefines FORK_ALLOC_COUNTERS=1 before any -Defines
#                     (allocation counts), extra_suffix bench[.<Variant>][.tests], so it never
#                     rebuilds or replaces the daily binaries:
#                     bin\godot.windows.<target>.x86_64.bench[.<Variant>].exe
#   ... -SourceRoot <dir>
#                     build another checkout (bench.py's git worktree of the previous
#                     commit): SCons, custom.py, bin\ and the --version call all use <dir>
#
# Run it from PowerShell or cmd (from Git Bash: powershell.exe -File ...), and
# close any editor running from bin\ first (the link fails with "Access is denied").
param(
	[ValidateSet("editor", "templates", "release", "template_release")]
	[string]$Target = "editor",
	[switch]$Mono,
	[int]$Jobs = 0,
	[switch]$KeepGoing,
	[switch]$Tests,
	[string]$Variant,
	[string]$Defines,
	[switch]$Dev,
	[switch]$Bench,
	[string]$SourceRoot
)
. "$PSScriptRoot\..\common.ps1"
if ($SourceRoot) { $RepoRoot = (Resolve-Path $SourceRoot).Path }
if ($Defines -and -not $Variant) { throw "-Defines needs -Variant <name>, so the candidate gets its own objects and binary and the control stays untouched." }
Set-Location $RepoRoot
# Git Bash sets these; platform/windows/detect.py then skips the D3D12 build_deps folder.
Remove-Item Env:MSYSTEM -ErrorAction SilentlyContinue
Remove-Item Env:MINGW_PREFIX -ErrorAction SilentlyContinue
$python = Get-SconsPython
$pythonExe = $python[0]
$pythonArgs = @($python | Select-Object -Skip 1)

if (-not (Test-Path (Join-Path $RepoRoot "custom.py"))) {
	Write-Host "No custom.py: building without PhysX, Blast and Flow. Run devtools\setup\setup_dev_windows.ps1 to set them up." -ForegroundColor Yellow
}
if ($Jobs -le 0) { $Jobs = [Environment]::ProcessorCount }
# extra_suffix without ".tests": the binary name part Get-Editor takes as -Variant.
$binVariant = (@($(if ($Bench) { "bench" }), $Variant) | Where-Object { $_ }) -join "."

function Invoke-Scons([string]$SconsTarget, [bool]$WithMono) {
	$sconsArgs = @("-m", "SCons", "-j$Jobs", "platform=windows", "target=$SconsTarget")
	$sconsArgs += $(if ($Dev) { "dev_build=yes" } else { "production=yes" })
	if ($WithMono) { $sconsArgs += "module_mono_enabled=yes" }
	if ($KeepGoing) { $sconsArgs += "-k" }
	if ($Tests) { $sconsArgs += @("tests=yes", "object_prefix=t_") }
	# debug_symbols is not part of the binary suffix: without the bench suffix it
	# would rebuild every object and replace the daily binaries.
	if ($Bench) { $sconsArgs += @("debug_symbols=yes", "disable_path_overrides=no") }
	$suffix = @($(if ($Bench) { "bench" }), $Variant, $(if ($Tests) { "tests" })) | Where-Object { $_ }
	if ($suffix) { $sconsArgs += "extra_suffix=$($suffix -join '.')" }
	# Bench builds always count allocations (core/os/fork_alloc_counters.h), so every
	# binary of a series carries the same counter cost.
	$cppDefines = @($(if ($Bench) { "FORK_ALLOC_COUNTERS=1" }), $Defines) | Where-Object { $_ }
	if ($cppDefines) { $sconsArgs += "cppdefines=$($cppDefines -join ' ')" }
	$label = "$SconsTarget$(if ($WithMono) { ' (.NET)' })"
	Write-Host "== Building $label" -ForegroundColor Cyan
	$start = Get-Date
	& $pythonExe @pythonArgs @sconsArgs
	if ($LASTEXITCODE -ne 0) { throw "Build of $label failed." }
	Write-Host ("   done in {0:N1} min" -f ((Get-Date) - $start).TotalMinutes) -ForegroundColor Green
}

function Build-MonoAssemblies {
	Write-Host "== C# glue and assemblies" -ForegroundColor Cyan
	& (Get-Editor -Mono -Tests:$Tests -Variant $binVariant -Dev:$Dev) --headless --generate-mono-glue modules\mono\glue
	if ($LASTEXITCODE -ne 0) { throw "Generating the C# glue failed." }
	& $pythonExe @pythonArgs modules/mono/build_scripts/build_assemblies.py --godot-output-dir=./bin --godot-platform=windows
	if ($LASTEXITCODE -ne 0) { throw "Building the C# assemblies failed." }
}

switch ($Target) {
	"editor" {
		Invoke-Scons "editor" $Mono
		if ($Mono) { Build-MonoAssemblies }
	}
	"template_release" {
		Invoke-Scons "template_release" $Mono
	}
	"templates" {
		Invoke-Scons "template_release" $Mono
		Invoke-Scons "template_debug" $Mono
	}
	"release" {
		# Commit (and bump CUSTOM_VERSION) first: the version stamp is the commit.
		foreach ($withMono in @($false, $true)) {
			Invoke-Scons "editor" $withMono
			Invoke-Scons "template_release" $withMono
			Invoke-Scons "template_debug" $withMono
		}
		Build-MonoAssemblies
	}
}
if ($Target -eq "template_release") {
	# Same name order as Get-Editor: target, ".dev", arch, extra_suffix, ".mono".
	$built = Join-Path $RepoRoot "bin\godot.windows.template_release$(if ($Dev) { '.dev' }).x86_64$(if ($binVariant) { ".$binVariant" })$(if ($Tests) { '.tests' })$(if ($Mono) { '.mono' }).console.exe"
	if (-not (Test-Path $built)) { throw "Missing $built after the build." }
	& $built --version
} else {
	& (Get-Editor -Mono:$Mono -Tests:$Tests -Variant $binVariant -Dev:$Dev) --version
}
