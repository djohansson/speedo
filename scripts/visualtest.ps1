<#
.SYNOPSIS
The client's frames checked against its own references (docs/visual-testing-plan.md, step 3).

.DESCRIPTION
Each check renders one scene twice, headless and repeatable (SPEEDO_REPEATABLE=1, with the exposure, the date and the
time pinned), as two views to compare: a debug view and its reference, or one setting and another. The first is the
reference, the second the capture, and build/<preset>/imagecompare measures the capture against the reference and fails
it past the check's thresholds. A check fails too where a client does (its log is kept). The captures and the
differences image are kept under <work>/<check>. Prints each check's result and how long it took: no time budget is set
yet, it is measured first (the user's call, 2026-11-11).

The thresholds leave a few times what was measured when each check was made (in its comment), so that they fail on a
real change and not on a run's noise: a check whose measures move should be looked at, not loosened.

-Only <names> runs those checks alone. The client needs a vulkan driver: on macOS the mesa port's KosmicKrisp unless
VK_DRIVER_FILES is set (lavapipe's, say). The ray query check needs a driver with ray queries other than KosmicKrisp
(two clients tracing, one after the other, which is more than the rule there allows without asking): lavapipe's.

.EXAMPLE
pwsh scripts/visualtest.ps1 -Preset arm64-osx-clang-profile -Work /tmp/visual
#>
[CmdletBinding()]
param(
	[Parameter(Mandatory = $true)] [string] $Preset,
	[string] $Work,
	[string[]] $Only, # not -Check: PowerShell's names are case-insensitive, and the loop's $check would overwrite it
	[string] $Size = '640x360',
	[int] $TimeoutSeconds = 600
)
$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$exe = if ($IsWindows) { '.exe' } else { '' }
$build = Join-Path $root "build/$Preset"
$clientExe = Join-Path $build "client$exe"
$compareExe = Join-Path $build "imagecompare$exe"
foreach ($tool in $clientExe, $compareExe)
{
	if (-not (Test-Path -LiteralPath $tool -PathType Leaf))
	{
		[Console]::Error.WriteLine("no ${tool}: build preset '$Preset' first")
		exit 2
	}
}
if (-not $Work)
{
	$Work = Join-Path ([IO.Path]::GetTempPath()) "speedo-visualtest-$PID"
}
New-Item -ItemType Directory -Force -Path $Work | Out-Null
$Work = (Resolve-Path -LiteralPath $Work).Path

# the install tree's libraries, as assettest.ps1 finds them
$config = $Preset.Substring($Preset.LastIndexOf('-') + 1)
$triplet = $Preset.Substring(0, $Preset.LastIndexOf('-'))
$lib = Join-Path $root ("install/$triplet/" + $(if ($config -eq 'debug') { 'debug/' } else { '' }) + $(if ($IsWindows) { 'bin' } else { 'lib' }))
$separator = [IO.Path]::PathSeparator
if ($IsMacOS)
{
	$env:DYLD_LIBRARY_PATH = if ($env:DYLD_LIBRARY_PATH) { "$lib$separator$env:DYLD_LIBRARY_PATH" } else { $lib }
}
elseif ($IsLinux)
{
	$env:LD_LIBRARY_PATH = if ($env:LD_LIBRARY_PATH) { "$lib$separator$env:LD_LIBRARY_PATH" } else { $lib }
}
else
{
	$env:PATH = "$lib$separator$env:PATH"
}
# the driver, unless one is chosen: on macOS the mesa port's KosmicKrisp, as assettest.ps1 takes it
if ($IsMacOS -and -not $env:VK_DRIVER_FILES -and -not $env:VK_ICD_FILENAMES)
{
	$icd = Get-ChildItem -Path (Join-Path $root 'install/*/share/vulkan/icd.d/kosmickrisp_mesa_icd.*.json') -ErrorAction SilentlyContinue | Select-Object -First 1
	if ($icd) { $env:VK_DRIVER_FILES = $icd.FullName }
}
$driver = "$env:VK_DRIVER_FILES$env:VK_ICD_FILENAMES"
$kosmicKrisp = $driver -match 'kosmickrisp'
Write-Host "driver: $(if ($driver) { $driver } else { 'the system''s' })"
Write-Host "work dir: $Work"

# what every run pins, so that a capture is the same from run to run (see docs/visual-testing-plan.md, step 1)
$pinned = [ordered]@{
	SPEEDO_REPEATABLE = '1'
	SPEEDO_HEADLESS = '1'
	SPEEDO_UI = '0'
	SPEEDO_WINDOW_SIZE = $Size
	SPEEDO_AUTO_EXPOSURE = '0'
	SPEEDO_EXPOSURE = '0'
	SPEEDO_TIME_FRAME_SECONDS = '0.0166667'
	SPEEDO_YEAR = '2026'
	SPEEDO_DAY_OF_YEAR = '172'
	SPEEDO_TIME_OF_DAY = '10:30'
	SPEEDO_LATITUDE = '57.7'
}

# the checks: a scene (a model, an environment, settings), the reference's variables and the capture's, imagecompare's
# thresholds, and what the check needs of the driver
$checks = @(
	[ordered]@{
		# the traced shadows as the main pass takes them (a ray a pixel, filtered over the penumbra) against many rays a
		# pixel over the light's disk: their edges differ, their light doesn't. ShadowScene lit by neutral.hdr, whose
		# softbox is the first traced light (the sky's sun casts nothing soft in view). measured 2026-11-11 on
		# KosmicKrisp: 0.004 stops blurred, -0.002 stops of light (before the shadow cache's fix, the floor's shadows
		# missing: 0.03 blurred)
		Name = 'traced-shadows'
		Model = 'test-models/gltf/ShadowScene.gltf'
		Environment = 'test-assets/glTF-Sample-Environments/neutral.hdr'
		Frames = 8
		Reference = @{ SPEEDO_DEBUG_VIEW = 'traced-shadow-reference' }
		Capture = @{ SPEEDO_DEBUG_VIEW = 'traced-shadow' }
		Thresholds = @('--log-blurred', '0.015', '--ratio', '0.01')
	}
	[ordered]@{
		# the frame with the traced shadows' default mode (traced where the shadow atlas isn't sure, the atlas's
		# elsewhere) against every pixel traced (SPEEDO_TRACED_SHADOWS=all, no atlas): the same where the atlas is right.
		# traced-shadows can't tell an atlas gone wrong, its views both trace only where the atlas isn't sure. this one
		# found the shadow cache keeping tiles cleared while their pipelines were made (the floor's shadows missing: 1.8%
		# of the pixels past 16 levels); measured 2026-11-11 on KosmicKrisp with that fixed: 0.035% past 16 levels
		Name = 'shadow-atlas'
		Model = 'test-models/gltf/ShadowScene.gltf'
		Environment = 'test-assets/glTF-Sample-Environments/neutral.hdr'
		Frames = 8
		Reference = @{ SPEEDO_TRACED_SHADOWS = 'all' }
		Capture = @{}
		Thresholds = @('--over', '16:0.5', '--log-blurred', '0.01', '--ratio', '0.01')
	}
	[ordered]@{
		# the emissive surfaces' light as the main pass takes it (filtered, accumulated) against many independent
		# estimates a traced pixel, nothing reused: the same light, spread differently (the reference's blocks of traced
		# pixels), so blurred wide. the CornellBox's eight boxes and their lamps, at an equal frame count (both views dim
		# together over longer runs, to look into). measured 2026-11-11 on KosmicKrisp: 0.085 stops blurred over 24
		# pixels, +0.002 stops of light
		Name = 'emitters'
		Model = 'test-assets/mcguire/CornellBox.zip'
		Frames = 32
		Reference = @{ SPEEDO_DEBUG_VIEW = 'emitters-reference' }
		Capture = @{ SPEEDO_DEBUG_VIEW = 'emitters' }
		Thresholds = @('--blur', '24', '--log-blurred', '0.2', '--ratio', '0.02')
	}
	[ordered]@{
		# temporal upscaling at native resolution against none: its edges and fine detail differ (the history's
		# reconstruction against the aliased frame), not its light. DamagedHelmet. measured 2026-11-11 on KosmicKrisp:
		# 0.7% of the pixels past 16 levels, 0.0075 stops blurred, -0.004 stops of light
		Name = 'upscaling'
		Model = 'test-assets/glTF-Sample-Assets/Models/DamagedHelmet/glTF/DamagedHelmet.gltf'
		Frames = 32
		Reference = @{ SPEEDO_UPSCALING = 'off' }
		Capture = @{ SPEEDO_UPSCALING = 'native' }
		Thresholds = @('--over', '16:2', '--log-blurred', '0.03', '--ratio', '0.02')
	}
)

# one client run: the scene's variables, its side's, and the pinned ones; returns whether it captured
function Invoke-Capture($Check, [string] $Side, [hashtable] $Variables, [string] $Directory)
{
	$capture = Join-Path $Directory "$Side.bmp"
	$log = Join-Path $Directory "$Side.log"
	Remove-Item -LiteralPath $capture -ErrorAction SilentlyContinue
	$set = [ordered]@{}
	foreach ($name in $pinned.Keys) { $set[$name] = $pinned[$name] }
	$set.SPEEDO_AUTOLOAD_MODEL = $Check.Model
	$set.SPEEDO_AUTOLOAD_EXIT = "$($Check.Frames)"
	$set.SPEEDO_CAPTURE = $capture
	if ($Check.Environment) { $set.SPEEDO_AUTOLOAD_ENVIRONMENT = $Check.Environment }
	foreach ($name in $Variables.Keys) { $set[$name] = $Variables[$name] }
	foreach ($name in $set.Keys) { [Environment]::SetEnvironmentVariable($name, $set[$name]) }
	try
	{
		$user = Join-Path $Work 'user'
		$process = Start-Process -FilePath $clientExe -ArgumentList '-u', "`"$user`"" -WorkingDirectory $root -PassThru -NoNewWindow `
			-RedirectStandardOutput $log -RedirectStandardError "$log.stderr"
	}
	finally
	{
		foreach ($name in $set.Keys) { [Environment]::SetEnvironmentVariable($name, $null) }
	}
	$null = $process.Handle
	if (-not $process.WaitForExit($TimeoutSeconds * 1000))
	{
		$process.Kill($true)
		$process.WaitForExit()
		Write-Host "  $Side timed out after $TimeoutSeconds s (log: $log)"
		return $false
	}
	if (Test-Path -LiteralPath "$log.stderr")
	{
		Get-Content -LiteralPath "$log.stderr" | Add-Content -LiteralPath $log
		Remove-Item -LiteralPath "$log.stderr" -ErrorAction SilentlyContinue
	}
	if ($process.ExitCode -ne 0 -or -not (Test-Path -LiteralPath $capture))
	{
		Write-Host "  $Side failed (exit $($process.ExitCode), log: $log)"
		return $false
	}
	return $true
}

$failed = 0
$ran = 0
foreach ($check in $checks)
{
	if ($Only -and $check.Name -notin $Only) { continue }
	if ($check.NeedsRayQueries -and $kosmicKrisp)
	{
		Write-Host "SKIP $($check.Name) (ray queries on KosmicKrisp: run it on lavapipe)"
		continue
	}
	$ran++
	$directory = Join-Path $Work $check.Name
	New-Item -ItemType Directory -Force -Path $directory | Out-Null
	$start = Get-Date
	$captured = (Invoke-Capture $check 'reference' $check.Reference $directory) -and (Invoke-Capture $check 'capture' $check.Capture $directory)
	$seconds = [int]((Get-Date) - $start).TotalSeconds
	if (-not $captured)
	{
		Write-Host "FAIL $($check.Name) ($seconds s): a client failed"
		$failed++
		continue
	}
	$arguments = @((Join-Path $directory 'reference.bmp'), (Join-Path $directory 'capture.bmp'), '--diff', (Join-Path $directory 'differences.bmp')) + $check.Thresholds
	$output = & $compareExe @arguments
	$exitCode = $LASTEXITCODE
	$output | Set-Content -LiteralPath (Join-Path $directory 'compare.txt')
	$summary = ($output | Where-Object { $_ -match '^mean \|log2\||^mean luminance ratio|^FAIL' }) -join '; '
	if ($exitCode -eq 0)
	{
		Write-Host "PASS $($check.Name) ($seconds s): $summary"
	}
	else
	{
		Write-Host "FAIL $($check.Name) ($seconds s): $summary"
		$failed++
	}
}
Write-Host "visual: $($ran - $failed) pass, $failed fail."
exit $(if ($failed -gt 0) { 1 } else { 0 })
