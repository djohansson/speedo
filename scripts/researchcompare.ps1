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

.EXAMPLE
pwsh scripts/researchcompare.ps1 -Preset x64-windows-clang-profile -Work build/researchcompare
#>
[CmdletBinding()]
param(
	[Parameter(Mandatory = $true)] [string] $Preset,
	[string] $Work,
	[string[]] $Only,
	[ValidateSet('extended', 'core')] [string] $Kind = 'extended',
	[int] $Frames = 1500,
	[int] $Jobs = 4,
	[int] $TimeoutSeconds = 900,
	[string[]] $Units,
	[string] $Review = 'temp/review/research',
	[string[]] $Set,
	[switch] $Validate
)
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

Add-Type -AssemblyName System.Drawing

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

# a png as a 24 bit bmp (imagecompare's input, as the client's captures)
function ConvertTo-Bmp([string] $Png, [string] $Bmp)
{
	$source = [Drawing.Bitmap]::new($Png)
	try
	{
		$target = [Drawing.Bitmap]::new($source.Width, $source.Height, [Drawing.Imaging.PixelFormat]::Format24bppRgb)
		$graphics = [Drawing.Graphics]::FromImage($target)
		$graphics.DrawImage($source, 0, 0, $source.Width, $source.Height)
		$graphics.Dispose()
		$target.Save($Bmp, [Drawing.Imaging.ImageFormat]::Bmp)
		$target.Dispose()
	}
	finally { $source.Dispose() }
}

# a row of images side by side at a width each, labelled, into a png
function New-Sheet([string[]] $Files, [string[]] $Labels, [int] $Width, [string] $Out)
{
	$images = @($Files | ForEach-Object { if ($_ -and (Test-Path -LiteralPath $_)) { [Drawing.Bitmap]::new($_) } else { $null } })
	$first = $images | Where-Object { $_ } | Select-Object -First 1
	$height = if ($first) { [int]($Width * $first.Height / $first.Width) } else { [int]($Width * 9 / 16) }
	$sheet = [Drawing.Bitmap]::new($Width * $Files.Count, $height + 22)
	$graphics = [Drawing.Graphics]::FromImage($sheet)
	$graphics.Clear([Drawing.Color]::Black)
	$graphics.InterpolationMode = [Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
	$font = [Drawing.Font]::new('Segoe UI', 10)
	for ($i = 0; $i -lt $Files.Count; $i++)
	{
		if ($images[$i]) { $graphics.DrawImage($images[$i], [Drawing.Rectangle]::new($i * $Width, 22, $Width, $height)) }
		$graphics.DrawString($Labels[$i], $font, [Drawing.Brushes]::White, [float]($i * $Width + 4), [float]3)
	}
	$graphics.Dispose()
	$sheet.Save($Out, [Drawing.Imaging.ImageFormat]::Png)
	$sheet.Dispose()
	foreach ($image in $images) { if ($image) { $image.Dispose() } }
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
	if ($image -and (Test-Path -LiteralPath $image)) { $bitmap = [Drawing.Image]::FromFile($image); $reference.Width = $bitmap.Width; $reference.Height = $bitmap.Height; $bitmap.Dispose() }
	[pscustomobject]@{ Name = $folder.Name; Folder = $folder.FullName; Model = $glb.FullName; Reference = $reference; Directory = (Join-Path $Work $folder.Name) }
}

# a scene's client run started: its variables set for it alone, then cleared
function Start-Scene($Scene, [int] $Slot)
{
	New-Item -ItemType Directory -Force -Path $Scene.Directory | Out-Null
	$capture = Join-Path $Scene.Directory 'capture.bmp'
	$log = Join-Path $Scene.Directory 'capture.log'
	Remove-Item -LiteralPath $capture -ErrorAction SilentlyContinue
	$reference = $Scene.Reference
	$set = [ordered]@{
		SPEEDO_HEADLESS = '1'; SPEEDO_UI = '0'; SPEEDO_SHADER_RELOAD = '0'; SPEEDO_REPEATABLE = '1'
		SPEEDO_WINDOW_SIZE = "$($reference.Width)x$($reference.Height)"
		SPEEDO_AUTOLOAD_MODEL = $Scene.Model; SPEEDO_AUTOLOAD_EXIT = "$Frames"; SPEEDO_CAPTURE = $capture
		SPEEDO_TIME_FRAME_SECONDS = '0.0166667'
		SPEEDO_LENS_GLARE = '0'; SPEEDO_MOTION_BLUR = '0'; SPEEDO_GRAIN = '0'; SPEEDO_UPSCALING = 'native'
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

# the runs, -Jobs at a time, each in a user directory of its slot's
$queue = [Collections.Generic.Queue[object]]::new([object[]]@($scenes))
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

# the overview: every scene's reference and capture, the most unlike first
$ordered = @($results | Sort-Object @{ Expression = { if ($null -ne $_.LogBlurred) { $_.LogBlurred } else { 99 } }; Descending = $true })
$cell = 320
$cellHeight = 180
$columns = 2
$overview = [Drawing.Bitmap]::new($cell * 2 * $columns, ($cellHeight + 20) * [Math]::Ceiling($ordered.Count / $columns))
$graphics = [Drawing.Graphics]::FromImage($overview)
$graphics.Clear([Drawing.Color]::Black)
$graphics.InterpolationMode = [Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
$font = [Drawing.Font]::new('Segoe UI', 9)
for ($i = 0; $i -lt $ordered.Count; $i++)
{
	$x = ($i % $columns) * $cell * 2
	$y = [Math]::Floor($i / $columns) * ($cellHeight + 20)
	$directory = Join-Path $Work $ordered[$i].Scene
	foreach ($side in @(@('reference.bmp', 0), @('capture.bmp', 1)))
	{
		$file = Join-Path $directory $side[0]
		if (Test-Path -LiteralPath $file) { $image = [Drawing.Bitmap]::new($file); $graphics.DrawImage($image, [Drawing.Rectangle]::new($x + $side[1] * $cell, $y + 20, $cell, $cellHeight)); $image.Dispose() }
	}
	$text = if ($null -ne $ordered[$i].LogBlurred) { '{0}: blurred {1:F3}, ratio {2:+0.00;-0.00} stops' -f $ordered[$i].Scene, $ordered[$i].LogBlurred, $ordered[$i].Ratio } else { "$($ordered[$i].Scene): $($ordered[$i].Status)" }
	$graphics.DrawString($text, $font, [Drawing.Brushes]::White, [float]($x + 4), [float]($y + 3))
}
$graphics.Dispose()
$overview.Save((Join-Path $Review 'overview.png'), [Drawing.Imaging.ImageFormat]::Png)
$overview.Dispose()

Write-Host ''
Write-Host 'the scenes most unlike their references first (mean |log2| luminance, blurred over 8 pixels: the lighting; the ratio: the light overall):'
foreach ($result in $ordered)
{
	$measures = if ($null -ne $result.LogBlurred) { 'blurred {0:F3}  sharp {1:F3}  ratio {2:+0.000;-0.000}  mean {3,6:F2} of 255' -f $result.LogBlurred, $result.Log, $result.Ratio, $result.Mean } else { $result.Status }
	Write-Host ("  {0,-18} {1}" -f $result.Scene, $measures)
}
Write-Host "sheets: $Review"
$failed = @($results | Where-Object { $_.Status -ne 'ok' -or $_.Problems }).Count
exit $(if ($failed -gt 0) { 1 } else { 0 })
