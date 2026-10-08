<#
.SYNOPSIS
Tests model and image loading on a set of assets.

.DESCRIPTION
The test sets are downloaded by scripts/fetch-test-assets.ps1, which prints their paths:
  scripts/assettest.ps1 -Client (scripts/fetch-test-assets.ps1)

Zip archives are extracted to <work>/assets/<archive name> (the work dir defaults to a new temporary dir, and is kept).
Then build/<preset>/assettest imports every model (.obj, .gltf, .glb) and image the way the client does and checks the
results (see src/tools/assettest.cpp). With -Client, the client also loads each model (and the first image next to it)
through the full load + upload + draw path, and a zip archive of several models (a set of variants), or a subdirectory
of a directory with several (e.g. a gltf sample model's encodings), as one, side by side (files the importers don't
support are skipped there), with a user profile dir under <work>, and fails a model if the client crashes or prints
load failures, failed asserts or validation errors (validation needs a debug preset). -ClientOnly skips the assettest
run (which is slow with a debug preset). The client needs a vulkan driver: set VK_DRIVER_FILES etc. as for running it
by hand. On macOS the display is woken and kept awake while the client runs (glfw finds no monitors while it sleeps).

The preset defaults to the first configured release preset (else any) in build/. The client exits SPEEDO_AUTOLOAD_EXIT
(default 30) frames after its loads have finished, and is killed after ASSETTEST_CLIENT_TIMEOUT (default 600) seconds.

Exits with 1 if anything failed. Logs are kept under <work>/logs.

.EXAMPLE
scripts/assettest.ps1 -ClientOnly -Preset arm64-osx-clang-debug -Work /tmp/work (scripts/fetch-test-assets.ps1)
#>
param(
	[switch] $Client,
	[switch] $ClientOnly,
	[string] $Preset,
	[string] $Work,
	[Parameter(ValueFromRemainingArguments = $true)] [string[]] $Inputs
)

$ErrorActionPreference = 'Stop'

if ($ClientOnly)
{
	$Client = $true
}
$check = -not $ClientOnly

if (-not $Inputs -or $Inputs.Count -eq 0)
{
	[Console]::Error.WriteLine('usage: scripts/assettest.ps1 [-Client | -ClientOnly] [-Preset <name>] [-Work <dir>] <zip or directory>...')
	exit 2
}

$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$exe = if ($IsWindows) { '.exe' } else { '' }

# sorted by their bytes, as the shell's tools did, rather than by culture
function Sort-Ordinal([string[]] $Values)
{
	$sorted = [string[]]@($Values)
	[Array]::Sort($sorted, [StringComparer]::Ordinal)
	return , $sorted
}

if (-not $Preset)
{
	# the first configured release preset, else any
	$presets = @(Get-ChildItem -Path (Join-Path $root 'build') -Directory -ErrorAction SilentlyContinue | ForEach-Object Name)
	$presets = Sort-Ordinal $presets
	$Preset = ($presets | Where-Object { $_ -like '*-release' } | Select-Object -First 1)
	if (-not $Preset)
	{
		$Preset = $presets | Select-Object -First 1
	}
}
$build = Join-Path $root "build/$Preset"
$assettest = Join-Path $build "assettest$exe"
if (-not (Test-Path -LiteralPath $assettest -PathType Leaf))
{
	[Console]::Error.WriteLine("no assettest in ${build}: build preset '$Preset' first")
	exit 2
}

# the vcpkg install tree for the preset's triplet (the preset name without its config)
$config = $Preset.Substring($Preset.LastIndexOf('-') + 1)
$triplet = $Preset.Substring(0, $Preset.LastIndexOf('-'))
$installDir = Join-Path $root "install/$triplet"
$lib = Join-Path $installDir ($(if ($config -eq 'debug') { 'debug/' } else { '' }) + $(if ($IsWindows) { 'bin' } else { 'lib' }))
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
if (-not $env:VK_LAYER_PATH)
{
	$env:VK_LAYER_PATH = Join-Path $installDir 'share/vulkan/explicit_layer.d'
}

if (-not $Work)
{
	$Work = Join-Path ([IO.Path]::GetTempPath()) ("assettest." + [IO.Path]::GetRandomFileName())
}
New-Item -ItemType Directory -Force -Path (Join-Path $Work 'assets'), (Join-Path $Work 'logs') | Out-Null
$Work = (Resolve-Path $Work).Path
Write-Output "work dir: $Work"

Add-Type -AssemblyName System.IO.Compression.FileSystem

# the model files under a directory, without macOS resource forks
function Get-Models([string] $Directory, [switch] $Shallow)
{
	$files = if ($Shallow) { Get-ChildItem -LiteralPath $Directory -File } else { Get-ChildItem -LiteralPath $Directory -File -Recurse }
	$files | Where-Object {
		$_.Extension -in '.obj', '.gltf', '.glb' -and -not $_.Name.StartsWith('._') -and
		$_.FullName -notmatch '[\\/]__MACOSX[\\/]'
	} | ForEach-Object FullName
}

# the first image (png, jpg, jpeg or tga) under a directory
function Get-FirstImage([string] $Directory, [switch] $Shallow)
{
	$files = if ($Shallow) { Get-ChildItem -LiteralPath $Directory -File } else { Get-ChildItem -LiteralPath $Directory -File -Recurse }
	$images = @($files | Where-Object { $_.Extension -in '.png', '.jpg', '.jpeg', '.tga' -and -not $_.Name.StartsWith('._') } |
		ForEach-Object FullName)
	(Sort-Ordinal $images) | Select-Object -First 1
}

$dirs = [System.Collections.Generic.List[string]]::new()
$setArchives = [System.Collections.Generic.List[string]]::new() # zip archives of several models (sets of variants), which the client loads as one, side by side
$setFolders = [System.Collections.Generic.List[string]]::new() # the same for the subdirectories of a directory, e.g. a gltf sample model's encodings
foreach ($item in $Inputs)
{
	if (Test-Path -LiteralPath $item -PathType Container)
	{
		$dir = (Get-Item -LiteralPath $item).ResolvedTarget
		if (-not $dir) { $dir = (Resolve-Path -LiteralPath $item).Path }
		$dirs.Add($dir)
		foreach ($sub in Get-ChildItem -LiteralPath $dir -Directory)
		{
			if (@(Get-Models $sub.FullName).Count -gt 1)
			{
				$setFolders.Add($sub.FullName)
			}
		}
	}
	elseif ($item -like '*.zip' -and (Test-Path -LiteralPath $item -PathType Leaf))
	{
		$name = [IO.Path]::GetFileNameWithoutExtension($item)
		$extracted = Join-Path $Work "assets/$name"
		if (-not (Test-Path -LiteralPath $extracted))
		{
			Write-Output "extracting $item"
			New-Item -ItemType Directory -Force -Path $extracted | Out-Null
			try
			{
				[System.IO.Compression.ZipFile]::ExtractToDirectory((Resolve-Path -LiteralPath $item).Path, $extracted, $true)
			}
			catch
			{
				[Console]::Error.WriteLine("failed to extract ${item}: $($_.Exception.Message)")
				exit 1
			}
		}
		$dirs.Add($extracted)
		if (@(Get-Models $extracted).Count -gt 1)
		{
			$setArchives.Add((Resolve-Path -LiteralPath $item).Path)
		}
	}
	else
	{
		[Console]::Error.WriteLine("not a zip archive or directory: $item")
		exit 2
	}
}

$failed = $false

if ($check)
{
	& $assettest @dirs | Tee-Object -FilePath (Join-Path $Work 'logs/assettest.log')
	if ($LASTEXITCODE -ne 0)
	{
		$failed = $true
	}
}

if ($Client)
{
	$clientExe = Join-Path $build "client$exe"
	if (-not (Test-Path -LiteralPath $clientExe -PathType Leaf))
	{
		[Console]::Error.WriteLine("no client in $build")
		exit 2
	}

	# the client exits this many frames after its loads have finished
	if (-not $env:SPEEDO_AUTOLOAD_EXIT)
	{
		$env:SPEEDO_AUTOLOAD_EXIT = '30'
	}
	# a small procedural sky (the default environment): the client's own takes seconds to prefilter, on every run
	if (-not $env:SPEEDO_SKY_WIDTH)
	{
		$env:SPEEDO_SKY_WIDTH = '256'
	}
	$timeout = if ($env:ASSETTEST_CLIENT_TIMEOUT) { [int]$env:ASSETTEST_CLIENT_TIMEOUT } else { 600 }
	$caffeinate = if ($IsMacOS) { Get-Command caffeinate -ErrorAction SilentlyContinue } else { $null }

	$script:pass = 0
	$script:fail = 0

	# runs the client on a model (a file, or a zip archive), drawn with image on the default material if given, and lit by
	# an environment panorama if given (else the procedural sky)
	function Invoke-Client([string] $Model, [string] $Image, [string] $Label, [string] $Environment = '')
	{
		$log = Join-Path $Work "logs/client-$Label.log"
		$errorLog = "$log.stderr"

		# glfw finds no monitors while the display sleeps (on macos), and the client then fails to start: wake it, and
		# keep it awake while the client runs
		if ($caffeinate)
		{
			& $caffeinate -u -t 1
		}

		$start = Get-Date
		$env:SPEEDO_AUTOLOAD_MODEL = $Model
		$env:SPEEDO_AUTOLOAD_IMAGE = $Image
		$env:SPEEDO_AUTOLOAD_ENVIRONMENT = $Environment
		try
		{
			$process = Start-Process -FilePath $clientExe -ArgumentList '-u', "`"$(Join-Path $Work 'user')`"" -PassThru -NoNewWindow `
				-RedirectStandardOutput $log -RedirectStandardError $errorLog
		}
		finally
		{
			Remove-Item Env:SPEEDO_AUTOLOAD_MODEL, Env:SPEEDO_AUTOLOAD_IMAGE, Env:SPEEDO_AUTOLOAD_ENVIRONMENT -ErrorAction SilentlyContinue
		}
		$null = $process.Handle # keeps the exit code available after the process exits (windows)
		$awake = if ($caffeinate) { Start-Process -FilePath $caffeinate.Source -ArgumentList '-d', '-i', '-w', $process.Id -PassThru } else { $null }

		$timedOut = -not $process.WaitForExit($timeout * 1000)
		if ($timedOut)
		{
			$process.Kill($true)
			$process.WaitForExit()
		}
		if ($awake -and -not $awake.HasExited)
		{
			$awake.Kill()
		}

		# one log: what the client printed to stdout, then to stderr
		if (Test-Path -LiteralPath $errorLog)
		{
			Get-Content -LiteralPath $errorLog | Add-Content -LiteralPath $log
			Remove-Item -LiteralPath $errorLog
		}
		if ($timedOut)
		{
			Add-Content -LiteralPath $log "timed out after ${timeout}s"
		}
		$status = $process.ExitCode
		$seconds = [int]((Get-Date) - $start).TotalSeconds

		$problems = @(Select-String -LiteralPath $log -Pattern 'Failed to load (model|image|archive|environment)|\(errno: |VUID-|UNASSIGNED-|timed out after' |
			ForEach-Object Line | Group-Object | Sort-Object Name | Select-Object -First 5 |
			ForEach-Object { '{0,7} {1}' -f $_.Count, $_.Name })
		if ($status -ne 0 -or $problems.Count -gt 0)
		{
			Write-Output "FAIL client $(if ($Model) { $Model } else { $Environment }) (exit $status, ${seconds}s, log: $log)"
			$problems | ForEach-Object { Write-Output "    $_" }
			$script:fail++
		}
		else
		{
			Write-Output "PASS client $(if ($Model) { $Model } else { $Environment }) (${seconds}s)"
			$script:pass++
		}
	}

	# an archive of several models is one run, with its first image (its textures can be missing, or its models leave
	# the default material to an image, as sphere.zip's do)
	$setDirs = [System.Collections.Generic.List[string]]::new()
	foreach ($archive in $setArchives)
	{
		$name = [IO.Path]::GetFileNameWithoutExtension($archive)
		$dir = Join-Path $Work "assets/$name"
		$setDirs.Add($dir)
		Invoke-Client $archive (Get-FirstImage $dir) $name
	}
	foreach ($folder in $setFolders)
	{
		$setDirs.Add($folder)
		Invoke-Client $folder '' ((Split-Path $folder -Leaf) -replace ' ', '_')
	}

	$models = Sort-Ordinal @($dirs | ForEach-Object { Get-Models $_ })
	foreach ($model in $models)
	{
		if ($setDirs | Where-Object { $model.StartsWith($_ + [IO.Path]::DirectorySeparatorChar) })
		{
			continue
		}

		# an obj model's textures can be missing, so it is drawn with an image next to it on the default material
		$dir = Split-Path $model
		$image = if ([IO.Path]::GetExtension($model) -eq '.obj') { Get-FirstImage $dir -Shallow } else { '' }
		Invoke-Client $model $image ("{0}-{1}" -f (Split-Path $dir -Leaf), ((Split-Path $model -Leaf) -replace ' ', '_'))
	}

	# the environment panoramas, each on its own
	$environments = Sort-Ordinal @($dirs | ForEach-Object {
		Get-ChildItem -LiteralPath $_ -File -Recurse -Filter '*.hdr' | Where-Object { -not $_.Name.StartsWith('._') } | ForEach-Object FullName })
	foreach ($environment in $environments)
	{
		Invoke-Client '' '' ("environment-{0}" -f [IO.Path]::GetFileNameWithoutExtension($environment)) $environment
	}

	Write-Output "client: $script:pass pass, $script:fail fail."
	if ($script:fail -gt 0)
	{
		$failed = $true
	}
}

exit [int]$failed
