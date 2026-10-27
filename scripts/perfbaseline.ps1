<#
.SYNOPSIS
The performance plan's measurement baseline (docs/performance-plan.md, step 1): what each feature and pass costs, and
how long the lighting takes to settle, at the benchmark poses.

.DESCRIPTION
Runs the client (build/<preset>/client) at each scene's pose (-Scenes: bistro, sanmiguel, sponza), at -Size framebuffer pixels
(SPEEDO_WINDOW_SIZE, default 2560x1440), in the background, fifo, without the ui:

-Features: everything on, each feature off in turn (indirect lighting, its near field and surface cache, the emitters,
the traced shadows, the shadows), all of them off, and the sun moving at 60 times real time; the default first and
last, to bound the gpu clock's drift.

-Passes: one pass left out at a time (SPEEDO_SKIP_PASSES; the GI pass by its steps), the default first, in the middle
and last. The culls and pre-passes stay in: leaving them out changes what everything after them draws. A pass's cost
is the frame time it frees, which a pass whose output decides another's work (the shadow atlas, the classification)
changes too.

-Convergence: from a cold start at the pose, still, at the exposure auto exposure settled on in a default run, every
-Stride-th of -ConvergenceFrames frames captured (SPEEDO_CAMERA_PATH, SPEEDO_CAPTURE_PATH), then build/<preset>/
convergence: the frames and seconds until the image stays within 10%, 5% and twice its noise of the settled one. The
captures (about 15 MB each at 2560x1440) are deleted afterwards unless -KeepCaptures.

Without any of the three, all of them. The measure is the draw thread's frame time (the median of its half seconds once
the lighting has warmed up: the surface cache's first 256 frames trace 4 rays a voxel, which the client times apart),
which is the gpu's where the draw thread waits on its fences (check with a sampling profiler): KosmicKrisp's
timestamps miss most compute passes, and the Frame scope (printed beside it) a third of a frame heavy with them. The
gpu's clock drifts between runs, so the defaults are measured more than once.

San Miguel loads from its extracted archive in the user profile (<root>/.speedo/archives/San_Miguel-*/san-miguel.obj):
open San_Miguel.zip in the client once if it isn't there. The client needs a vulkan driver (VK_DRIVER_FILES etc. as for
running it by hand). Logs and captures go to -Work (default a new temporary directory); the tables are printed and
written to <work>/baseline.md.

.EXAMPLE
scripts/perfbaseline.ps1 -Preset arm64-osx-clang-profile -Scenes bistro -Features
#>
[CmdletBinding(PositionalBinding = $false)]
param(
	[string] $Preset,
	[string] $Work,
	[string[]] $Scenes = @('bistro', 'sanmiguel', 'sponza'),
	[switch] $Features,
	[switch] $Passes,
	[switch] $Convergence,
	[int] $Frames = 600,
	[int] $ConvergenceFrames = 1200,
	[int] $Stride = 10,
	[string] $Size = '2560x1440',
	[switch] $KeepCaptures
)

$ErrorActionPreference = 'Stop'

if (-not ($Features -or $Passes -or $Convergence))
{
	$Features = $true
	$Passes = $true
	$Convergence = $true
}

$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$exe = if ($IsWindows) { '.exe' } else { '' }

if (-not $Preset)
{
	# the first configured profile preset (its gpu timings), else any
	$presets = @(Get-ChildItem -Path (Join-Path $root 'build') -Directory -ErrorAction SilentlyContinue | ForEach-Object Name | Sort-Object)
	$Preset = ($presets | Where-Object { $_ -like '*-profile' } | Select-Object -First 1)
	if (-not $Preset)
	{
		$Preset = $presets | Select-Object -First 1
	}
}
$build = Join-Path $root "build/$Preset"
$client = Join-Path $build "client$exe"
$convergenceTool = Join-Path $build "convergence$exe"
foreach ($tool in @($client) + $(if ($Convergence) { @($convergenceTool) } else { @() }))
{
	if (-not (Test-Path -LiteralPath $tool -PathType Leaf))
	{
		[Console]::Error.WriteLine("no $(Split-Path -Leaf $tool) in ${build}: build preset '$Preset' first")
		exit 2
	}
}

# the vcpkg install tree's libraries for the preset's triplet (as scripts/assettest.ps1 finds them)
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

if (-not $Work)
{
	$Work = Join-Path ([IO.Path]::GetTempPath()) "speedo-perfbaseline-$(Get-Date -Format 'yyyyMMdd-HHmmss')"
}
$null = New-Item -ItemType Directory -Force -Path $Work
$report = Join-Path $Work 'baseline.md'

# the benchmark poses (the user's, see docs/performance-plan.md)
$sanMiguel = Get-ChildItem -Path (Join-Path $root '.speedo/archives') -Filter 'san-miguel.obj' -Recurse -Depth 1 -ErrorAction SilentlyContinue |
	Where-Object { $_.Directory.Name -like 'San_Miguel-*' } | Select-Object -First 1
$sceneTable = @{
	bistro = @{
		Model = Join-Path $root 'resources/test-assets/mcguire/bistro/Exterior/exterior.obj'
		Camera = '-800.3088,154.0938,132.3450,-4.860,71.352'
		Time = '20.000'
	}
	sanmiguel = @{
		Model = if ($sanMiguel) { $sanMiguel.FullName } else { '' }
		Camera = '8.5157,1.6398,2.3221,-6.609,-251.093'
		Time = '10.500'
	}
	sponza = @{
		Model = Join-Path $root 'resources/test-assets/glTF-Sample-Assets/Models/Sponza/glTF/Sponza.gltf'
		Camera = '-8.6117,0.7801,3.0412,0.609,-269.373'
		Time = '10.500'
	}
}

$caffeinate = if ($IsMacOS) { Get-Command caffeinate -ErrorAction SilentlyContinue } else { $null }
$timeout = 1800

# runs the client at a scene's pose with the variables given (and the common ones), its log at $Log; returns what it
# printed of its timings
function Invoke-Client([hashtable] $Scene, [hashtable] $Variables, [string] $Log)
{
	$common = @{
		SPEEDO_BACKGROUND = '1'; SPEEDO_PRESENT_MODE = 'fifo'; SPEEDO_UI = '0'; SPEEDO_GPU_TIMINGS = '1'; SPEEDO_GI = '1'
		SPEEDO_WINDOW_SIZE = $Size; SPEEDO_AUTOLOAD_MODEL = $Scene.Model; SPEEDO_CAMERA = $Scene.Camera
		SPEEDO_TIME_OF_DAY = $Scene.Time; SPEEDO_AUTOLOAD_EXIT = "$Frames"
	}
	$all = $common.Clone()
	foreach ($name in $Variables.Keys)
	{
		$all[$name] = $Variables[$name]
	}
	$saved = @{}
	foreach ($name in $all.Keys)
	{
		$saved[$name] = [Environment]::GetEnvironmentVariable($name)
		[Environment]::SetEnvironmentVariable($name, $(if ($all[$name] -eq '') { $null } else { $all[$name] }))
	}
	try
	{
		# glfw finds no monitors while the display sleeps (on macos): woken, and kept awake while the client runs
		if ($caffeinate) { & $caffeinate -u -t 1 }
		$process = Start-Process -FilePath $client -PassThru -NoNewWindow -RedirectStandardOutput $Log -RedirectStandardError "$Log.stderr"
		$null = $process.Handle # keeps the exit code available after the process exits (windows)
		$awake = if ($caffeinate) { Start-Process -FilePath $caffeinate.Source -ArgumentList '-d', '-i', '-w', $process.Id -PassThru } else { $null }
		if (-not $process.WaitForExit($timeout * 1000))
		{
			$process.Kill($true)
			$process.WaitForExit()
			Add-Content -LiteralPath $Log "timed out after ${timeout}s"
		}
		if ($awake -and -not $awake.HasExited) { $awake.Kill() }
	}
	finally
	{
		foreach ($name in $saved.Keys)
		{
			[Environment]::SetEnvironmentVariable($name, $saved[$name])
		}
	}
	if (Test-Path -LiteralPath "$Log.stderr")
	{
		Get-Content -LiteralPath "$Log.stderr" | Add-Content -LiteralPath $Log
		Remove-Item -LiteralPath "$Log.stderr" -ErrorAction SilentlyContinue
	}
	$text = Get-Content -LiteralPath $Log -Raw
	$number = { param($pattern) if ($text -match $pattern) { [double]$Matches[1] } else { [double]::NaN } }
	return [pscustomobject]@{
		Exit = $process.ExitCode
		Draw = & $number '(?m)^frame time: ([0-9.]+) ms'
		Warm = & $number '(?m)^frame time warming up: ([0-9.]+) ms'
		FrameScope = & $number '(?m)^  Frame: ([0-9.]+)'
		Exposure = & $number '(?m)^auto exposure: ([-+0-9.]+) EV'
		Log = $Log
	}
}

function Write-Report([string] $Line)
{
	Write-Host $Line
	Add-Content -LiteralPath $report $Line
}

function Format-Ms([double] $Value)
{
	if ([double]::IsNaN($Value)) { return 'n/a' }
	return $Value.ToString('0.0', [Globalization.CultureInfo]::InvariantCulture)
}

# a table of runs against the mean of the default runs among them
function Write-Table([string] $Title, [string] $Column, [object[]] $Rows)
{
	$defaults = @($Rows | Where-Object { $_.Default -and $_.Result.Exit -eq 0 } | ForEach-Object { $_.Result.Draw })
	$default = if ($defaults.Count -gt 0) { ($defaults | Measure-Object -Average).Average } else { [double]::NaN }
	Write-Report ''
	Write-Report "### $Title"
	Write-Report ''
	Write-Report "| $Column | frame time (ms) | frees (ms) | Frame scope (ms) |"
	Write-Report '|---|---|---|---|'
	foreach ($row in $Rows)
	{
		$result = $row.Result
		$frees = if ($row.Default) { '' } elseif ($result.Exit -ne 0) { "exit $($result.Exit)" } else { Format-Ms ($default - $result.Draw) }
		Write-Report "| $($row.Name) | $(Format-Ms $result.Draw) | $frees | $(Format-Ms $result.FrameScope) |"
	}
	return $default
}

Write-Report "## Baseline: $Preset, $Size, $(Get-Date -Format 'yyyy-MM-dd HH:mm')"
foreach ($sceneName in $Scenes)
{
	$scene = $sceneTable[$sceneName]
	if (-not $scene)
	{
		[Console]::Error.WriteLine("unknown scene '$sceneName' (bistro, sanmiguel, sponza)")
		exit 2
	}
	if (-not $scene.Model -or -not (Test-Path -LiteralPath $scene.Model))
	{
		[Console]::Error.WriteLine("${sceneName}: no model at '$($scene.Model)' (San Miguel: open San_Miguel.zip in the client once)")
		continue
	}
	Write-Report ''
	Write-Report "## $sceneName (SPEEDO_CAMERA=$($scene.Camera) SPEEDO_TIME_OF_DAY=$($scene.Time))"
	$defaultResult = $null

	if ($Features)
	{
		$variants = @(
			@{ Name = 'everything on'; Variables = @{}; Default = $true }
			@{ Name = 'indirect lighting off'; Variables = @{ SPEEDO_GI = '0' } }
			@{ Name = 'near field off'; Variables = @{ SPEEDO_GI_NEAR = '0' } }
			@{ Name = 'surface cache off'; Variables = @{ SPEEDO_GI_CACHE = '0' } }
			@{ Name = 'emitters off'; Variables = @{ SPEEDO_EMISSIVE_LIGHTS = '0' } }
			@{ Name = 'traced shadows off'; Variables = @{ SPEEDO_TRACED_SHADOWS = '0' } }
			@{ Name = 'shadows off'; Variables = @{ SPEEDO_SHADOWS = '0' } }
			@{ Name = 'all of them off'; Variables = @{ SPEEDO_GI = '0'; SPEEDO_SHADOWS = '0'; SPEEDO_EMISSIVE_LIGHTS = '0' } }
			@{ Name = 'the sun moving (60x)'; Variables = @{ SPEEDO_TIME_SPEED = '60' } }
			@{ Name = 'everything on'; Variables = @{}; Default = $true }
		)
		$rows = @()
		$index = 0
		foreach ($variant in $variants)
		{
			$index++
			$result = Invoke-Client $scene $variant.Variables (Join-Path $Work "$sceneName-feature-$index.log")
			Write-Host "  $($variant.Name): $(Format-Ms $result.Draw) ms"
			$rows += [pscustomobject]@{ Name = $variant.Name; Default = [bool]$variant.Default; Result = $result }
			if ($variant.Default -and -not $defaultResult) { $defaultResult = $result }
		}
		$null = Write-Table 'Features' 'run' $rows
	}

	if ($Passes)
	{
		$passList = @(
			'', 'Depth Pyramid', 'Shadows', 'Shadow Classify', 'Shadow Trace', 'Shadow Denoise X', 'Shadow Denoise Y',
			'GI Light', 'GI Flags', 'GI Trace', 'GI Probe Update', 'GI Cache Release', 'GI Resolve', 'GI Cache Filter',
			'', 'GI Near', 'GI Near X', 'GI Near Y', 'GI Near Temporal',
			'Emitter Sample', 'Emitter Shade', 'Emitter X', 'Emitter Y', 'Emitter Temporal',
			'Main', 'Main Late', 'ComputeMain', ''
		)
		$rows = @()
		$index = 0
		foreach ($pass in $passList)
		{
			$index++
			$result = Invoke-Client $scene @{ SPEEDO_SKIP_PASSES = $pass } (Join-Path $Work "$sceneName-pass-$index.log")
			$name = if ($pass) { $pass } else { '(none)' }
			Write-Host "  without ${name}: $(Format-Ms $result.Draw) ms"
			$rows += [pscustomobject]@{ Name = $name; Default = -not $pass; Result = $result }
			if (-not $pass -and -not $defaultResult) { $defaultResult = $result }
		}
		$null = Write-Table 'Passes left out' 'pass' $rows
	}

	if ($Convergence)
	{
		if (-not $defaultResult)
		{
			$defaultResult = Invoke-Client $scene @{} (Join-Path $Work "$sceneName-default.log")
		}
		$exposure = if ([double]::IsNaN($defaultResult.Exposure)) { 0.0 } else { $defaultResult.Exposure }
		$captures = Join-Path $Work "captures-$sceneName"
		Remove-Item -LiteralPath $captures -Recurse -Force -ErrorAction SilentlyContinue
		$null = New-Item -ItemType Directory -Force -Path $captures
		# the pose a frame, in a file (an environment variable on windows holds 32767 characters)
		$path = Join-Path $Work "path-$sceneName.txt"
		Set-Content -LiteralPath $path -Value (@($scene.Camera) * $ConvergenceFrames)
		$result = Invoke-Client $scene @{
			SPEEDO_CAMERA = ''; SPEEDO_CAMERA_PATH = "@$path"; SPEEDO_CAPTURE_PATH = $captures
			SPEEDO_CAPTURE_PATH_STRIDE = "$Stride"; SPEEDO_AUTOLOAD_EXIT = '10'; SPEEDO_GPU_TIMINGS = ''
			SPEEDO_AUTO_EXPOSURE = '0'; SPEEDO_EXPOSURE = $exposure.ToString([Globalization.CultureInfo]::InvariantCulture)
		} (Join-Path $Work "$sceneName-convergence.log")
		Write-Report ''
		Write-Report "### Convergence (from a cold start, at $($exposure.ToString('+0.00;-0.00', [Globalization.CultureInfo]::InvariantCulture)) EV, every ${Stride}th of $ConvergenceFrames frames)"
		Write-Report ''
		if ($result.Exit -ne 0)
		{
			Write-Report "the capture run failed (exit $($result.Exit)): see $($result.Log)"
		}
		else
		{
			$frameMs = if ([double]::IsNaN($defaultResult.Draw)) { 0 } else { $defaultResult.Draw }
			Write-Report '```'
			# the warm-up's frames at their own time (the renderer's kGiCacheWarmFrameCount)
			$warm = if ([double]::IsNaN($defaultResult.Warm)) { @() } else { @('--warm-ms', $defaultResult.Warm.ToString([Globalization.CultureInfo]::InvariantCulture), '--warm-frames', '256') }
			& $convergenceTool $captures --frame-ms $frameMs.ToString([Globalization.CultureInfo]::InvariantCulture) @warm | ForEach-Object { Write-Report $_ }
			Write-Report '```'
		}
		if (-not $KeepCaptures)
		{
			Remove-Item -LiteralPath $captures -Recurse -Force -ErrorAction SilentlyContinue
		}
	}
}
Write-Host ''
Write-Host "written to $report"
