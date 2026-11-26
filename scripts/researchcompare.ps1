<#
.SYNOPSIS
The glTF research scenes rendered as their references were, each beside its reference.

.DESCRIPTION
Each scene of the glTF research set (resources/test-assets/gltf-research-scenes, fetch-test-assets.ps1's) comes with a
reference render of its glTF, a path tracer's (Blender Cycles', cornell-caustic's pbrt's), and how it was made in its
conversion.yaml (reference_render): the resolution, the environment (a panorama at a strength, a uniform color, or none),
the exposure multiplier, Khronos PBR Neutral, the file's camera. The client takes those conditions from the yaml itself
as it opens the file (windowedapplication::ReadModelConditions: the panorama at its strength, a uniform color, or none;
a manual exposure of the multiplier; PBR Neutral; no ground plane; docs/research-scenes-plan.md, A1), and the
panorama's convention is the set's (A2), so this renders each scene as the client opens it: the file's first camera
(the client's default), at the reference image's size, headless and repeatable, the camera's own effects off (glare,
motion blur, grain; the default lens is a pinhole, without depth of field), antialiased by the temporal upscaler at
native size. Then build/<preset>/imagecompare measures it against the reference, and a sheet of the two and their
differences (amplified 16 times) goes to -Review/<scene>.png, with an overview of every scene's pair
(-Review/overview.png) and a table of the measures, the scenes most unlike their references first.

Nothing passes or fails here but the clients: a path tracer's reference shows what the renderer approximates, which is
what this maps (docs/research-scenes-plan.md), not a regression.

-Set <name=value> adds variables to every run (an experiment's). -Kind core renders the files without the material
extensions. -Only <scenes> renders those alone. -Units <scene=unit>
gives a scene's unit (SPEEDO_UNITS) where it isn't meters. -Jobs clients run at once, each with a user directory of its
own under -Work (their pipelines kept between runs). -Validate loads the validation layer, as visualtest.ps1 does.

-Timings times each scene after the captures, one client at a time, at the research plan's target (-TimingSize,
2560x1440, with the default upscaling, balanced; the same conditions and pose otherwise, the camera's effects off as in
the captures), -TimingFrames frames: the frame time (the client's median of the half seconds after the lighting warmed
up, SPEEDO_GPU_TIMINGS=1, its pipelines warm from the captures) and the costliest passes' gpu times, in the table and
the overview. Not with -Validate (the layer's own time).

-Chart <path> writes the overview as a JPEG there too, for people: each pair labelled (the reference's renderer, speedo's
graphics api), how much brighter or darker speedo's image is, the frame time, under a line saying what made them (the
operating system, the graphics api, the gpu: -Device, else the system's name of it, and how rays were traced). Beside it,
<path> with .md, the measures in full (both |log2| differences, the light's ratio, the mean difference, the frame times
and costliest passes): docs/research-scenes/overview.jpg and overview.md, which the plan tracks its progress by
(docs/research-scenes-plan.md).

-Reuse runs no client: it measures, sheets and charts what -Work holds from an earlier run (its captures, and with
-Timings its timing logs).

-Archive <dir> keeps each scene's capture as <dir>/<scene>.jpg (JPEG quality 92) with the measures beside them
(measures.md): a platform's and backend's set, docs/research-scenes/captures/<platform>-<backend> (macos-metal,
windows-d3d12, linux-vulkan, ...), from which a comparison of platforms can be made later.

-KeepCaptures captures only the scenes without a capture in -Work (<scene>/capture.bmp, at the reference's size, made by
an earlier run or put there by hand) and measures the rest as -Reuse does; the timings still run for every scene.

.EXAMPLE
pwsh scripts/researchcompare.ps1 -Preset x64-windows-clang-profile -Work build/researchcompare

.EXAMPLE
pwsh scripts/researchcompare.ps1 -Preset x64-windows-clang-profile -Work build/researchcompare -Timings -Chart docs/research-scenes/overview.jpg
#>
[CmdletBinding()]
param(
	[Parameter(Mandatory = $true)] [string] $Preset,
	[string] $Work,
	[string[]] $Only,
	[ValidateSet('extended', 'core')] [string] $Kind = 'extended',
	[int] $Frames = 300,
	[int] $Jobs = 4,
	[int] $TimeoutSeconds = 900,
	[string[]] $Units,
	[string] $Review = 'temp/review/research',
	[string[]] $Set,
	[switch] $Validate,
	[switch] $Timings,
	[string] $TimingSize = '2560x1440',
	[int] $TimingFrames = 900,
	[string] $Chart,
	[string] $Device,
	[switch] $Reuse,
	[switch] $KeepCaptures,
	[string] $Archive
)
if ($Timings -and $Validate) { [Console]::Error.WriteLine('-Timings without -Validate: the layer would be timed too'); exit 2 }
if (-not $env:SPEEDO_TRACY) { $env:SPEEDO_TRACY = '0' }
$ErrorActionPreference = 'Stop'
$Only = @($Only | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
$unitOf = @{}
foreach ($pair in @($Units | ForEach-Object { $_ -split ',' } | Where-Object { $_ })) { $name, $unit = $pair -split '=', 2; $unitOf[$name] = $unit }
# -Set: variables of every run's, after the scene's (an experiment: SPEEDO_TRACED_REFRACTIONS=0, say)
$extra = [ordered]@{}
foreach ($pair in @($Set | ForEach-Object { $_ -split ',' } | Where-Object { $_ })) { $name, $value = $pair -split '=', 2; $extra[$name] = $value }

$root = Split-Path -Parent $PSScriptRoot
$exe = if ($IsWindows) { '.exe' } else { '' }
$build = Join-Path $root "build/$Preset"
$clientExe = Join-Path $build "client$exe"
$compareExe = Join-Path $build "imagecompare$exe"
$sheetExe = Join-Path $build "imagesheet$exe"
$fontFile = Join-Path $root 'resources/fonts/Roboto-Medium.ttf' # the client's ui font, for the sheets' and chart's labels
foreach ($tool in $clientExe, $compareExe)
{
	if (-not (Test-Path -LiteralPath $tool -PathType Leaf)) { [Console]::Error.WriteLine("no ${tool}: build preset '$Preset' first"); exit 2 }
}
$assets = if ($env:SPEEDO_TEST_ASSETS) { $env:SPEEDO_TEST_ASSETS } else { Join-Path $root 'resources/test-assets' }
$scenesRoot = Join-Path $assets 'gltf-research-scenes/scenes/gltf_research_scenes_glb/gltf_research_scenes/scenes'
if (-not (Test-Path -LiteralPath $scenesRoot)) { [Console]::Error.WriteLine("no research scenes at ${scenesRoot}: fetch-test-assets.ps1 first"); exit 2 }
if (-not $Work) { $Work = Join-Path $build 'researchcompare' }
New-Item -ItemType Directory -Force -Path $Work | Out-Null
$Work = (Resolve-Path -LiteralPath $Work).Path
$Review = if ([IO.Path]::IsPathRooted($Review)) { $Review } else { Join-Path $root $Review }
New-Item -ItemType Directory -Force -Path $Review | Out-Null

# the install tree's libraries and -Validate's layer, as visualtest.ps1 takes them
$config = $Preset.Substring($Preset.LastIndexOf('-') + 1)
$triplet = $Preset.Substring(0, $Preset.LastIndexOf('-'))
$lib = Join-Path $root ("install/$triplet/" + $(if ($config -eq 'debug') { 'debug/' } else { '' }) + $(if ($IsWindows) { 'bin' } else { 'lib' }))
$separator = [IO.Path]::PathSeparator
if ($IsMacOS) { $env:DYLD_LIBRARY_PATH = if ($env:DYLD_LIBRARY_PATH) { "$lib$separator$env:DYLD_LIBRARY_PATH" } else { $lib } }
elseif ($IsLinux) { $env:LD_LIBRARY_PATH = if ($env:LD_LIBRARY_PATH) { "$lib$separator$env:LD_LIBRARY_PATH" } else { $lib } }
else { $env:PATH = "$lib$separator$env:PATH" }
$validating = $Validate -and $config -ne 'debug'
if ($Validate -and -not $env:VK_LAYER_PATH) { $env:VK_LAYER_PATH = if ($IsWindows) { $lib } else { Join-Path $root "install/$triplet/share/vulkan/explicit_layer.d" } }
if ($validating)
{
	$layer = 'VK_LAYER_KHRONOS_validation'
	$env:VK_INSTANCE_LAYERS = if ($env:VK_INSTANCE_LAYERS) { "$layer$separator$env:VK_INSTANCE_LAYERS" } else { $layer }
	foreach ($setting in @(@('VALIDATE_CORE', 'true'), @('THREAD_SAFETY', 'true'), @('DEBUG_ACTION', 'VK_DBG_LAYER_ACTION_LOG_MSG'), @('REPORT_FLAGS', 'info,warn,perf,error'),
			@('ENABLE_MESSAGE_LIMIT', 'true'), @('DUPLICATE_MESSAGE_LIMIT', '3'), @('MESSAGE_ID_FILTER', 'WARNING-CreateInstance-debug-warning')))
	{
		if (-not [Environment]::GetEnvironmentVariable("VK_KHRONOS_VALIDATION_$($setting[0])")) { [Environment]::SetEnvironmentVariable("VK_KHRONOS_VALIDATION_$($setting[0])", $setting[1]) }
	}
}
if (-not $env:SPEEDO_GRAPHICS_API) { $env:SPEEDO_GRAPHICS_API = if ($IsMacOS) { 'metal' } else { 'vulkan' } }
if ($Validate -and $env:SPEEDO_GRAPHICS_API -eq 'd3d12' -and -not $env:SPEEDO_D3D12_VALIDATION) { $env:SPEEDO_D3D12_VALIDATION = '1' }
if ($IsMacOS -and -not $env:VK_DRIVER_FILES -and -not $env:VK_ICD_FILENAMES)
{
	$icd = Get-ChildItem -Path (Join-Path $root 'install/*/share/vulkan/icd.d/kosmickrisp_mesa_icd.*.json') -ErrorAction SilentlyContinue | Select-Object -First 1
	if ($icd) { $env:VK_DRIVER_FILES = $icd.FullName }
}
$problemPattern = 'Failed to load (model|image|archive|environment)|\(errno: |VUID-|UNASSIGNED-|Validation (Error|Warning): |WARNING-CoreValidation-|ERROR: (Address|Thread)Sanitizer|runtime error: |WARNING: ThreadSanitizer|failed assertion `|Metal: command buffer .* failed'
Write-Host "graphics api: $env:SPEEDO_GRAPHICS_API"
Write-Host "work dir: $Work"

# a conversion.yaml's reference_render: its image, size, environment and its strength, exposure multiplier
function Read-Reference([string] $Yaml)
{
	$reference = @{ Image = $null; Width = 1280; Height = 720; Environment = $null; Strength = 1.0; Exposure = 1.0; Tonemapper = $null; Renderer = $null }
	$inside = $false
	foreach ($line in Get-Content -LiteralPath $Yaml)
	{
		if ($line -match '^reference_render:') { $inside = $true; continue }
		if (-not $inside) { continue }
		if ($line -match '^\S') { break }
		if ($line -match '^  image: "?([^"]+)"?') { $reference.Image = $Matches[1] }
		elseif ($line -match '^  renderer: "?([^"]+)"?') { $reference.Renderer = $Matches[1] }
		elseif ($line -match '^  resolution: \[(\d+), (\d+)\]') { $reference.Width = [int]$Matches[1]; $reference.Height = [int]$Matches[2] }
		elseif ($line -match '^  environment_map: (.+)$') { $value = $Matches[1].Trim().Trim('"'); $reference.Environment = if ($value -eq 'null') { $null } else { $value } }
		elseif ($line -match '^  environment_strength: (.+)$') { $value = $Matches[1].Trim(); if ($value -ne 'null') { $reference.Strength = [double]::Parse($value, [Globalization.CultureInfo]::InvariantCulture) } }
		elseif ($line -match '^  exposure_multiplier: (.+)$') { $reference.Exposure = [double]::Parse($Matches[1].Trim(), [Globalization.CultureInfo]::InvariantCulture) }
		elseif ($line -match '^  tonemapper: "?([^"]+)"?') { $reference.Tonemapper = $Matches[1] }
	}
	return $reference
}

# the images are made by build/<preset>/imagesheet (src/tools/imagesheet.cpp: sizes, conversions and compositions, the
# same on every platform)

# an image's size, @(width, height), or $null
function Get-ImageSize([string] $File)
{
	$size = & $sheetExe size $File
	if ($LASTEXITCODE -ne 0 -or -not $size) { return $null }
	return @($size -split ' ' | ForEach-Object { [int]$_ })
}

# a png as a bmp (imagecompare's input, as the client's captures)
function ConvertTo-Bmp([string] $Png, [string] $Bmp)
{
	New-Item -ItemType Directory -Force -Path (Split-Path -Parent $Bmp) | Out-Null
	& $sheetExe bmp $Png $Bmp
	if ($LASTEXITCODE -ne 0) { throw "imagesheet couldn't convert $Png" }
}

# a row of images side by side at a width each, labelled, into a png
function New-Sheet([string[]] $Files, [string[]] $Labels, [int] $Width, [string] $Out)
{
	$first = $Files | Where-Object { $_ -and (Test-Path -LiteralPath $_) } | Select-Object -First 1
	$firstSize = if ($first) { Get-ImageSize $first } else { $null }
	$height = if ($firstSize) { [int]($Width * $firstSize[1] / $firstSize[0]) } else { [int]($Width * 9 / 16) }
	$items = [Collections.Generic.List[string]]::new()
	for ($i = 0; $i -lt $Files.Count; $i++)
	{
		if ($Files[$i] -and (Test-Path -LiteralPath $Files[$i])) { $items.AddRange([string[]]@('--image', $Files[$i], "$($i * $Width)", '22', "$Width", "$height")) }
		$items.AddRange([string[]]@('--text', "$($i * $Width + 4)", '3', "$($Width - 8)", '15', $Labels[$i]))
	}
	& $sheetExe compose ($Width * $Files.Count) ($height + 22) --font $fontFile @items --out $Out
	if ($LASTEXITCODE -ne 0) { Write-Warning "imagesheet couldn't make $Out" }
}

# the scenes: each folder with a conversion.yaml and a glb of the kind (the core one where the kind's is missing)
$scenes = foreach ($folder in Get-ChildItem -LiteralPath $scenesRoot -Directory | Sort-Object Name)
{
	$yaml = Join-Path $folder.FullName 'gltf/conversion.yaml'
	if (-not (Test-Path -LiteralPath $yaml)) { continue }
	if ($Only -and $folder.Name -notin $Only) { continue }
	$glb = Get-ChildItem -LiteralPath (Join-Path $folder.FullName 'gltf') -Filter "*_$Kind.glb" | Select-Object -First 1
	if (-not $glb) { $glb = Get-ChildItem -LiteralPath (Join-Path $folder.FullName 'gltf') -Filter '*.glb' | Select-Object -First 1 }
	if (-not $glb) { continue }
	$reference = Read-Reference $yaml
	# the reference image's own size where it has one (cornell-caustic's pbrt render is 1024x1024, which its notes don't say)
	$image = if ($reference.Image) { Join-Path $folder.FullName "gltf/$($reference.Image)" } else { $null }
	$imageSize = if ($image -and (Test-Path -LiteralPath $image)) { Get-ImageSize $image } else { $null }
	if ($imageSize) { $reference.Width = $imageSize[0]; $reference.Height = $imageSize[1] }
	[pscustomobject]@{ Name = $folder.Name; Folder = $folder.FullName; Model = $glb.FullName; Reference = $reference; Directory = (Join-Path $Work $folder.Name) }
}

# a scene's client run started: its variables set for it alone, then cleared. -Timing: the timing run (see -Timings), at
# -TimingSize with the default upscaling, the gpu's timings in its log, no capture
function Start-Scene($Scene, [int] $Slot, [switch] $Timing)
{
	New-Item -ItemType Directory -Force -Path $Scene.Directory | Out-Null
	$capture = Join-Path $Scene.Directory 'capture.bmp'
	$log = Join-Path $Scene.Directory $(if ($Timing) { 'timing.log' } else { 'capture.log' })
	if (-not $Timing) { Remove-Item -LiteralPath $capture -ErrorAction SilentlyContinue }
	$reference = $Scene.Reference
	$set = [ordered]@{
		SPEEDO_HEADLESS = '1'; SPEEDO_UI = '0'; SPEEDO_SHADER_RELOAD = '0'; SPEEDO_REPEATABLE = '1'
		SPEEDO_WINDOW_SIZE = "$($reference.Width)x$($reference.Height)"
		SPEEDO_AUTOLOAD_MODEL = $Scene.Model; SPEEDO_AUTOLOAD_EXIT = "$Frames"; SPEEDO_CAPTURE = $capture
		SPEEDO_TIME_FRAME_SECONDS = '0.0166667'
		SPEEDO_LENS_GLARE = '0'; SPEEDO_MOTION_BLUR = '0'; SPEEDO_GRAIN = '0'; SPEEDO_UPSCALING = 'native'
	}
	if ($Timing)
	{
		$set.SPEEDO_WINDOW_SIZE = $TimingSize
		$set.SPEEDO_AUTOLOAD_EXIT = "$TimingFrames"
		$set.SPEEDO_GPU_TIMINGS = '1'
		$set.Remove('SPEEDO_CAPTURE')
		$set.Remove('SPEEDO_UPSCALING')
	}
	if ($unitOf.ContainsKey($Scene.Name)) { $set.SPEEDO_UNITS = $unitOf[$Scene.Name] }
	foreach ($pair in $extra.Keys) { $set[$pair] = $extra[$pair] }
	if ($validating) { Remove-Item -LiteralPath "$log.validation" -ErrorAction SilentlyContinue; $set.VK_KHRONOS_VALIDATION_LOG_FILENAME = "$log.validation" }
	foreach ($name in $set.Keys) { [Environment]::SetEnvironmentVariable($name, $set[$name]) }
	try
	{
		$user = Join-Path $Work "user$Slot"
		$process = Start-Process -FilePath $clientExe -ArgumentList '-u', "`"$user`"" -WorkingDirectory $root -PassThru -NoNewWindow `
			-RedirectStandardOutput $log -RedirectStandardError "$log.stderr"
	}
	finally
	{
		foreach ($name in $set.Keys) { [Environment]::SetEnvironmentVariable($name, $null) }
	}
	$null = $process.Handle
	return [pscustomobject]@{ Scene = $Scene; Process = $process; Slot = $Slot; Start = Get-Date; Log = $log; Capture = $capture }
}

# a finished run: its log joined, then the capture measured against the reference and set beside it
function Complete-Scene($Run, [bool] $TimedOut)
{
	$scene = $Run.Scene
	foreach ($part in "$($Run.Log).stderr", "$($Run.Log).validation")
	{
		if (Test-Path -LiteralPath $part) { Get-Content -LiteralPath $part | Add-Content -LiteralPath $Run.Log; Remove-Item -LiteralPath $part -ErrorAction SilentlyContinue }
	}
	$seconds = [int]((Get-Date) - $Run.Start).TotalSeconds
	$result = [ordered]@{ Scene = $scene.Name; Seconds = $seconds; Status = 'ok'; Log = $null; LogBlurred = $null; Ratio = $null; Mean = $null; Problems = '' }
	if ($TimedOut) { $result.Status = "timed out after $TimeoutSeconds s" }
	elseif ($Run.Process.ExitCode -ne 0 -or -not (Test-Path -LiteralPath $Run.Capture)) { $result.Status = "client failed (exit $($Run.Process.ExitCode))" }
	$problems = @(Select-String -LiteralPath $Run.Log -Pattern $problemPattern -ErrorAction SilentlyContinue | ForEach-Object Line | Group-Object | Sort-Object Name | Select-Object -First 3 | ForEach-Object { "$($_.Count)x $($_.Name)" })
	if ($problems.Count -gt 0) { $result.Problems = $problems -join ' | ' }
	$referencePng = if ($scene.Reference.Image) { Join-Path $scene.Folder "gltf/$($scene.Reference.Image)" } else { $null }
	$referenceBmp = Join-Path $scene.Directory 'reference.bmp'
	$differences = Join-Path $scene.Directory 'differences.bmp'
	if ($referencePng -and (Test-Path -LiteralPath $referencePng)) { ConvertTo-Bmp $referencePng $referenceBmp } else { $referenceBmp = $null }
	if ($result.Status -eq 'ok' -and $referenceBmp)
	{
		$output = & $compareExe $referenceBmp $Run.Capture --diff $differences
		$output | Set-Content -LiteralPath (Join-Path $scene.Directory 'compare.txt')
		foreach ($line in $output)
		{
			if ($line -match '^mean \|log2\| luminance: ([\d.]+) stops, ([\d.]+) blurred') { $result.Log = [double]::Parse($Matches[1], [Globalization.CultureInfo]::InvariantCulture); $result.LogBlurred = [double]::Parse($Matches[2], [Globalization.CultureInfo]::InvariantCulture) }
			elseif ($line -match '^mean luminance ratio: ([+-][\d.]+) stops') { $result.Ratio = [double]::Parse($Matches[1], [Globalization.CultureInfo]::InvariantCulture) }
			elseif ($line -match 'mean r ([\d.]+) g ([\d.]+) b ([\d.]+)') { $result.Mean = [Math]::Round(([double]::Parse($Matches[1], [Globalization.CultureInfo]::InvariantCulture) + [double]::Parse($Matches[2], [Globalization.CultureInfo]::InvariantCulture) + [double]::Parse($Matches[3], [Globalization.CultureInfo]::InvariantCulture)) / 3, 2) }
			elseif ($line -match '^sizes differ') { $result.Status = $line }
		}
	}
	$label = "reference ($(if ($scene.Reference.Renderer) { $scene.Reference.Renderer } else { 'Cycles' }))"
	New-Sheet @($referenceBmp, $(if (Test-Path -LiteralPath $Run.Capture) { $Run.Capture }), $(if (Test-Path -LiteralPath $differences) { $differences })) @($label, "speedo ($Kind)", 'differences x16') 640 (Join-Path $Review "$($scene.Name).png")
	$measures = if ($null -ne $result.LogBlurred) { 'log {0:F3}, blurred {1:F3}, ratio {2:+0.000;-0.000} stops' -f $result.Log, $result.LogBlurred, $result.Ratio } else { 'not measured' }
	Write-Host ("{0,-18} {1,4} s  {2}: {3}{4}" -f $scene.Name, $seconds, $result.Status, $measures, $(if ($result.Problems) { "  [$($result.Problems)]" } else { '' }))
	return [pscustomobject]$result
}

# the runs, -Jobs at a time, each in a user directory of its slot's (-Reuse: none, what -Work holds measured as if each
# had just run)
$kept = @($scenes | Where-Object { $Reuse -or ($KeepCaptures -and (Test-Path -LiteralPath (Join-Path $_.Directory 'capture.bmp'))) })
$queue = [Collections.Generic.Queue[object]]::new([object[]]@($scenes | Where-Object { $_ -notin $kept }))
$running = [Collections.Generic.List[object]]::new()
$results = [Collections.Generic.List[object]]::new()
$free = [Collections.Generic.Queue[int]]::new([int[]](1..$Jobs))
while ($queue.Count -gt 0 -or $running.Count -gt 0)
{
	while ($queue.Count -gt 0 -and $free.Count -gt 0)
	{
		$scene = $queue.Dequeue()
		$slot = $free.Dequeue()
		$run = Start-Scene $scene $slot
		if ($run) { $running.Add($run) } else { $free.Enqueue($slot) }
	}
	Start-Sleep -Milliseconds 500
	foreach ($run in @($running))
	{
		$timedOut = -not $run.Process.HasExited -and ((Get-Date) - $run.Start).TotalSeconds -gt $TimeoutSeconds
		if ($timedOut) { $run.Process.Kill($true); $run.Process.WaitForExit() }
		if ($run.Process.HasExited)
		{
			$running.Remove($run) | Out-Null
			$free.Enqueue($run.Slot)
			$results.Add((Complete-Scene $run $timedOut))
		}
	}
}
# the scenes not run (-Reuse, or -KeepCaptures with a capture already there): what -Work holds measured as if each had
# just run
foreach ($scene in $kept)
{
	$run = [pscustomobject]@{
		Scene = $scene; Process = [pscustomobject]@{ ExitCode = 0 }; Start = Get-Date
		Log = Join-Path $scene.Directory 'capture.log'; Capture = Join-Path $scene.Directory 'capture.bmp'
	}
	$results.Add((Complete-Scene $run $false))
}

# the timings (-Timings): each scene again, one client at a time in the first slot's user directory (its pipelines warm
# from the captures), at the target; the frame time and the costliest passes (see below)
foreach ($result in $results)
{
	$result | Add-Member -NotePropertyName FrameMs -NotePropertyValue $null -Force
	$result | Add-Member -NotePropertyName Passes -NotePropertyValue '' -Force
}
if ($Timings)
{
	Write-Host ''
	Write-Host "timings at $TimingSize, balanced upscaling, $TimingFrames frames, one client at a time:"
	foreach ($scene in $scenes)
	{
		$result = $results | Where-Object Scene -eq $scene.Name | Select-Object -First 1
		$log = Join-Path $scene.Directory 'timing.log'
		$exit = 0
		if (-not $Reuse)
		{
			$run = Start-Scene $scene 1 -Timing
			if (-not $run.Process.WaitForExit($TimeoutSeconds * 1000)) { $run.Process.Kill($true); $run.Process.WaitForExit() }
			$exit = $run.Process.ExitCode
		}
		# the frame time: the client's median of the half seconds after the lighting warmed up (the wall's, as 60 Hz is),
		# not the gpu timings' mean over every frame, which takes in the near empty frames drawn while a panorama loads
		# (classroom2's 2377 frames for its scene's 993 passes: 1.65 ms where the frames took 13.5); the passes' means over
		# the frames they ran in
		$lines = @(Get-Content -LiteralPath $log -ErrorAction SilentlyContinue)
		$settled = $lines | Select-String -Pattern '^frame time: ([0-9.]+) ms' | Select-Object -First 1
		$frame = $lines | Select-String -Pattern '^  Frame: ([0-9.]+) \((\d+) frames\)' | Select-Object -First 1
		if ($settled -and $frame)
		{
			$result.FrameMs = [double]::Parse($settled.Matches[0].Groups[1].Value, [Globalization.CultureInfo]::InvariantCulture)
			$passes = @(foreach ($line in $lines)
			{
				if ($line -match '^  (.+): ([0-9.]+) \((\d+) frames\)$' -and $Matches[1] -ne 'Frame')
				{
					[pscustomobject]@{ Name = $Matches[1]; Ms = [double]::Parse($Matches[2], [Globalization.CultureInfo]::InvariantCulture); Frames = [int]$Matches[3] }
				}
			})
			# (those of at least half the frames the scene was drawn in, Cull's: not its one-off voxelizing; a pass run in the
			# frames before it was there too, a panorama loading, by its time over the scene's frames, the others' near none)
			$cull = $passes | Where-Object Name -eq 'Cull' | Select-Object -First 1
			$sceneFrames = if ($cull) { $cull.Frames } else { ($passes | Measure-Object -Property Frames -Maximum).Maximum }
			foreach ($pass in $passes) { if ($pass.Frames -gt $sceneFrames) { $pass.Ms = $pass.Ms * $pass.Frames / $sceneFrames } }
			$result.Passes = (@($passes | Where-Object { $_.Frames -ge $sceneFrames / 2 } | Sort-Object Ms -Descending | Select-Object -First 3) | ForEach-Object { [string]::Format([Globalization.CultureInfo]::InvariantCulture, '{0} {1:F2}', $_.Name, $_.Ms) }) -join ', '
			Write-Host ("{0,-18} {1,6:F2} ms {2,5:F0} Hz  {3}" -f $scene.Name, $result.FrameMs, (1000.0 / $result.FrameMs), $result.Passes)
		}
		else
		{
			Write-Host ("{0,-18} not timed (exit {1}, log: {2})" -f $scene.Name, $exit, $log)
		}
	}
}

# the overview: every scene's reference and capture (each kept in shape in its cell), the most unlike first, under a line
# saying what it is of
$ordered = @($results | Sort-Object @{ Expression = { if ($null -ne $_.LogBlurred) { $_.LogBlurred } else { 99 } }; Descending = $true })
$cell = 320
$cellHeight = 240
$label = 38 # the scene's line, then which image is which
$header = 80
$columns = 2
$overviewWidth = $cell * 2 * $columns
$overviewHeight = $header + ($cellHeight + $label) * [Math]::Ceiling($ordered.Count / $columns)
$items = [Collections.Generic.List[string]]::new()
if (-not $Device -and $IsWindows)
{
	$Device = (Get-CimInstance Win32_VideoController -ErrorAction SilentlyContinue | Where-Object { $_.Name -match 'NVIDIA|AMD|Radeon|Intel\(R\) Arc' } | Select-Object -First 1).Name
}
elseif (-not $Device -and $IsMacOS)
{
	$Device = (& system_profiler SPDisplaysDataType 2>$null | Select-String 'Chipset Model: (.+)' | Select-Object -First 1).Matches.Groups[1].Value
}
$commit = (& git -C $root rev-parse --short HEAD 2>$null)
# what made the captures and the frame times, on a line of its own: the operating system, the graphics api, the gpu and
# how rays were traced (the clients' logs' "ray queries:" line: the hardware's or gfx's BVHs)
$platform = if ($IsMacOS) { "macOS $(& sw_vers -productVersion 2>$null)" }
	elseif ($IsWindows) { "Windows $([Environment]::OSVersion.Version)" }
	else { ((Get-Content -LiteralPath '/etc/os-release' -ErrorAction SilentlyContinue | Where-Object { $_ -like 'PRETTY_NAME=*' }) -replace '^PRETTY_NAME="?([^"]*)"?$', '$1') }
$apiNames = @{ vulkan = 'Vulkan'; metal = 'Metal'; d3d12 = 'D3D12' }
$api = if ($apiNames.ContainsKey("$env:SPEEDO_GRAPHICS_API")) { $apiNames["$env:SPEEDO_GRAPHICS_API"] } else { "$env:SPEEDO_GRAPHICS_API" }
$rayQueries = @($ordered | ForEach-Object { Join-Path (Join-Path $Work $_.Scene) 'capture.log' } | Where-Object { Test-Path -LiteralPath $_ } |
	ForEach-Object { Select-String -LiteralPath $_ -Pattern '^ray queries: (.+)$' | Select-Object -First 1 } | ForEach-Object { $_.Matches[0].Groups[1].Value } | Sort-Object -Unique)
$rays = switch -Regex ($rayQueries -join ', ') { "^the hardware's$" { 'hardware ray tracing' } '^$' { 'ray tracing not logged' } default { "ray queries: $_" } }
$madeOn = "Captured$(if ($Timings) { ' and timed' }) on $(@($platform, $api, $Device, $rays | Where-Object { $_ }) -join ', ')"
$title = "speedo ($Kind) against the glTF research set's path traced references, the scenes most unlike their references first. " +
	"Each scene: how much brighter or darker speedo's image is overall than the reference$(if ($Timings) { ", and the frame time at $TimingSize with balanced upscaling" }). " +
	"$(Get-Date -Format 'yyyy-MM-dd'), $commit."
$items.AddRange([string[]]@('--text', '6', '6', "$($overviewWidth - 12)", '22', $madeOn))
$items.AddRange([string[]]@('--text', '6', '36', "$($overviewWidth - 12)", '15', $title))
for ($i = 0; $i -lt $ordered.Count; $i++)
{
	$x = ($i % $columns) * $cell * 2
	$y = $header + [Math]::Floor($i / $columns) * ($cellHeight + $label)
	$directory = Join-Path $Work $ordered[$i].Scene
	foreach ($side in @(@('reference.bmp', 0), @('capture.bmp', 1)))
	{
		$file = Join-Path $directory $side[0]
		if (-not (Test-Path -LiteralPath $file)) { continue }
		# (fit to the cell, its shape kept and centered: imagesheet's --image)
		$items.AddRange([string[]]@('--image', $file, "$($x + $side[1] * $cell)", "$($y + $label)", "$cell", "$cellHeight"))
	}
	# (in the invariant culture's numbers: the chart goes to docs, whatever the machine's locale)
	$invariant = [Globalization.CultureInfo]::InvariantCulture
	# the overall light against the reference's as a share (the ratio is log2 of the mean linear luminances' quotient)
	$text = if ($null -ne $ordered[$i].Ratio)
	{
		$share = ([Math]::Pow(2.0, $ordered[$i].Ratio) - 1.0) * 100.0
		$light = if ([Math]::Abs($share) -lt 0.5) { 'as bright' } elseif ($share -gt 0) { [string]::Format($invariant, '{0:F0}% brighter', $share) } else { [string]::Format($invariant, '{0:F0}% darker', -$share) }
		"$($ordered[$i].Scene): $light"
	}
	else { "$($ordered[$i].Scene): $($ordered[$i].Status)" }
	if ($null -ne $ordered[$i].FrameMs) { $text += [string]::Format($invariant, ', {0:F1} ms', $ordered[$i].FrameMs) }
	$items.AddRange([string[]]@('--text', "$($x + 6)", "$($y + 3)", "$($cell * 2 - 12)", '15', $text))
	# which image is which: the reference's renderer (its conversion.yaml's, Cycles where it names none) and speedo's
	$sceneInfo = $scenes | Where-Object Name -eq $ordered[$i].Scene | Select-Object -First 1
	$renderer = if ($sceneInfo -and $sceneInfo.Reference.Renderer) { $sceneInfo.Reference.Renderer } else { 'Blender Cycles' }
	$items.AddRange([string[]]@('--text', "$($x + 6)", "$($y + 21)", "$($cell - 12)", '13', "reference ($renderer)"))
	$items.AddRange([string[]]@('--text', "$($x + $cell + 6)", "$($y + 21)", "$($cell - 12)", '13', "speedo ($api)"))
}
$items.AddRange([string[]]@('--out', (Join-Path $Review 'overview.png')))
if ($Chart)
{
	$chartPath = if ([IO.Path]::IsPathRooted($Chart)) { $Chart } else { Join-Path $root $Chart }
	New-Item -ItemType Directory -Force -Path (Split-Path -Parent $chartPath) | Out-Null
	$items.AddRange([string[]]@('--out', $chartPath, '--quality', '88'))
}
& $sheetExe compose $overviewWidth $overviewHeight --font $fontFile @items
if ($LASTEXITCODE -ne 0) { Write-Warning "imagesheet couldn't make the overview" }
elseif ($Chart) { Write-Host "chart: $chartPath" }

Write-Host ''
Write-Host 'the scenes most unlike their references first (mean |log2| luminance, blurred over 8 pixels: the lighting; the ratio: the light overall):'
foreach ($result in $ordered)
{
	$measures = if ($null -ne $result.LogBlurred) { 'blurred {0:F3}  sharp {1:F3}  ratio {2:+0.000;-0.000}  mean {3,6:F2} of 255' -f $result.LogBlurred, $result.Log, $result.Ratio, $result.Mean } else { $result.Status }
	if ($null -ne $result.FrameMs) { $measures += '  {0,6:F2} ms' -f $result.FrameMs }
	Write-Host ("  {0,-18} {1}" -f $result.Scene, $measures)
}
Write-Host "sheets: $Review"

# the measures in full, for reading and comparing rather than looking at: the chart is for people, this for the record (in
# the invariant culture's numbers, as the chart). $Beside says what the file is written with
function Write-Measures([string] $Path, [string] $Beside)
{
	$invariant = [Globalization.CultureInfo]::InvariantCulture
	$lines = [Collections.Generic.List[string]]::new()
	$lines.Add('# Research scenes: measures')
	$lines.Add('')
	$lines.Add("speedo ($Kind) against the glTF research set's path traced references, written by scripts/researchcompare.ps1 $Beside. $madeOn; $(Get-Date -Format 'yyyy-MM-dd'), commit $commit.")
	$lines.Add('')
	$lines.Add("- **Blurred** and **sharp**: the mean |log2| difference of the two images' linear luminances, in stops (1 is twice or half the light), blurred over 8 pixels (the lighting's error rather than its noise) and per pixel. The scenes are ordered by the blurred one, the most unlike first.")
	$lines.Add("- **Light**: log2 of speedo's mean linear luminance over the reference's, in stops: the light overall (+1 twice as bright, -1 half). The chart shows it as a share brighter or darker.")
	$lines.Add('- **Mean**: the mean channel difference, in levels of 255.')
	if ($Timings) { $lines.Add("- **Frame**: the client's median frame time at $TimingSize with balanced upscaling after the lighting settled, and the costliest passes' gpu times, in ms.") }
	$lines.Add('')
	$lines.Add("| Scene | Blurred | Sharp | Light | Mean | $(if ($Timings) { 'Frame | Costliest passes |' } else { '' })")
	$lines.Add("|---|---:|---:|---:|---:|$(if ($Timings) { '---:|---|' } else { '' })")
	foreach ($result in $ordered)
	{
		$row = if ($null -ne $result.LogBlurred) { [string]::Format($invariant, '| {0} | {1:F3} | {2:F3} | {3:+0.000;-0.000} | {4:F2} |', $result.Scene, $result.LogBlurred, $result.Log, $result.Ratio, $result.Mean) } else { "| $($result.Scene) | $($result.Status) | | | |" }
		if ($Timings) { $row += if ($null -ne $result.FrameMs) { [string]::Format($invariant, ' {0:F2} | {1} |', $result.FrameMs, $result.Passes) } else { ' | |' } }
		$lines.Add($row)
	}
	Set-Content -LiteralPath $Path -Value $lines -Encoding utf8NoBOM
	Write-Host "measures: $Path"
}
if ($Chart) { Write-Measures ([IO.Path]::ChangeExtension($chartPath, '.md')) "with the chart beside it ($([IO.Path]::GetFileName($chartPath)))" }

# -Archive: each scene's capture kept as <dir>/<scene>.jpg (quality 92), with the measures beside them (measures.md): a
# platform's and backend's set (docs/research-scenes/captures/<platform>-<backend>), to compare platforms by later
if ($Archive)
{
	$archivePath = if ([IO.Path]::IsPathRooted($Archive)) { $Archive } else { Join-Path $root $Archive }
	New-Item -ItemType Directory -Force -Path $archivePath | Out-Null
	$kept = 0
	foreach ($result in $ordered)
	{
		$capture = Join-Path (Join-Path $Work $result.Scene) 'capture.bmp'
		$size = if (Test-Path -LiteralPath $capture) { Get-ImageSize $capture } else { $null }
		if (-not $size) { continue }
		& $sheetExe compose $size[0] $size[1] --image $capture 0 0 $size[0] $size[1] --out (Join-Path $archivePath "$($result.Scene).jpg") --quality 92
		if ($LASTEXITCODE -eq 0) { $kept++ } else { Write-Warning "imagesheet couldn't keep $($result.Scene)'s capture" }
	}
	Write-Measures (Join-Path $archivePath 'measures.md') 'with the captures beside it (<scene>.jpg, at the reference''s size, JPEG quality 92)'
	Write-Host "archive: $kept captures in $archivePath"
}
$failed = @($results | Where-Object { $_.Status -ne 'ok' -or $_.Problems }).Count
exit $(if ($failed -gt 0) { 1 } else { 0 })
