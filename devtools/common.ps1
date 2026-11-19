# Shared by the devtools scripts (dot-source it: . "$PSScriptRoot\..\common.ps1").
# Paths are derived from this file's location, never hard-coded.
$ErrorActionPreference = "Stop"
$DevRoot = (Resolve-Path "$PSScriptRoot").Path
$RepoRoot = (Resolve-Path "$DevRoot\..").Path
$Out = Join-Path $DevRoot "out"
New-Item -ItemType Directory -Force $Out | Out-Null

function Get-Editor([switch]$Mono, [switch]$Tests) {
	# SConstruct puts extra_suffix (".tests") before the module suffix (".mono").
	$suffix = "$(if ($Tests) { '.tests' })$(if ($Mono) { '.mono' })"
	$exe = Join-Path $RepoRoot "bin\godot.windows.editor.x86_64$suffix.console.exe"
	if (-not (Test-Path $exe)) { throw "Missing $exe. Build it first: devtools\build\build.ps1$(if ($Mono) { ' -Mono' })$(if ($Tests) { ' -Tests' })" }
	return $exe
}

# The Python that has SCons: "python" on PATH first, then every interpreter the
# py launcher knows, newest first. Returns @(executable, leading args...).
# Never installs anything (see kb/mistakes/python-build-interpreter.md).
function Get-SconsPython {
	$candidates = @(, @("python"))
	if (Get-Command py -ErrorAction SilentlyContinue) {
		$versions = & py -0p 2>$null | ForEach-Object { if ($_ -match '-V:(\d+)\.(\d+)') { [version]"$($Matches[1]).$($Matches[2])" } }
		foreach ($v in @($versions | Sort-Object -Descending -Unique)) { $candidates += , @("py", "-$($v.Major).$($v.Minor)") }
	}
	foreach ($candidate in $candidates) {
		if (-not (Get-Command $candidate[0] -ErrorAction SilentlyContinue)) { continue }
		$leading = @($candidate | Select-Object -Skip 1)
		$ErrorActionPreference = "Continue"
		& $candidate[0] @leading -m SCons --version *> $null
		if ($LASTEXITCODE -eq 0) { return $candidate }
	}
	throw "No Python with SCons found; see kb/mistakes/python-build-interpreter.md"
}

# Lines that start with ERROR, minus the ones every build of this fork prints
# (AccessKit, the screen reader driver, is not part of the build).
function Get-RealErrors([string[]]$Lines) {
	return @($Lines | Where-Object { $_ -match "^ERROR" -and $_ -notmatch "screen reader support driver" })
}

function Write-Result([string]$Name, [bool]$Pass, [string]$Detail) {
	$mark = if ($Pass) { "PASS" } else { "FAIL" }
	$color = if ($Pass) { "Green" } else { "Red" }
	Write-Host ("{0}  {1,-34} {2}" -f $mark, $Name, $Detail) -ForegroundColor $color
}
