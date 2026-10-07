<#
.SYNOPSIS
Downloads the asset sets that scripts/assettest.ps1 tests the model and image loaders on.

.DESCRIPTION
-McGuire: Morgan McGuire's Computer Graphics Archive (https://casual-effects.com/data), obj models with their materials
and textures, as zip archives (about 2.7 GB), into <dir>/mcguire. Each file is checked against the size and sha256 in
scripts/test-assets/mcguire.txt: the archive publishes no versions, so a file that changed upstream is reported (and
kept as <name>.unverified) rather than used, until the manifest is updated.

-Gltf: the Khronos glTF-Sample-Assets models (https://github.com/KhronosGroup/glTF-Sample-Assets), at the commit pinned
below (about 2.3 GB), into <dir>/glTF-Sample-Assets.

-Environments: the Khronos glTF-Sample-Environments panoramas (https://github.com/KhronosGroup/glTF-Sample-Environments,
the .hdr files the glTF Sample Viewer lights with), at the commit pinned below (about 400 MB), into
<dir>/glTF-Sample-Environments. They are Git LFS files, so they are downloaded one by one from GitHub's LFS media
server and checked against the size and sha256 of their LFS pointers, in scripts/test-assets/environments.txt.

Without any of them, all are fetched. The hand-made models in scripts/test-assets/gltf and scripts/test-assets/obj (for
what no downloaded model covers, e.g. sparse index accessors, or bump map tangents) are always printed too. The dir defaults to $env:SPEEDO_TEST_ASSETS, or
resources/test-assets (which git ignores, and the client's file dialogs open in). Files already there (and verified)
are kept, so running it again only fetches what is missing.

Prints the paths to pass to scripts/assettest.ps1 (progress and errors go to stderr), e.g.
  scripts/assettest.ps1 -Client (scripts/fetch-test-assets.ps1)
Exits with 1 if anything failed.

.EXAMPLE
scripts/fetch-test-assets.ps1 -Gltf
#>
param(
	[string] $Dir,
	[switch] $McGuire,
	[switch] $Gltf,
	[switch] $Environments
)

$ErrorActionPreference = 'Stop'
# Invoke-WebRequest's progress display slows large downloads down a lot
$ProgressPreference = 'SilentlyContinue'

$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
if (-not $Dir)
{
	$Dir = if ($env:SPEEDO_TEST_ASSETS) { $env:SPEEDO_TEST_ASSETS } else { Join-Path $root 'resources/test-assets' }
}
if (-not $McGuire -and -not $Gltf -and -not $Environments)
{
	$McGuire = $true
	$Gltf = $true
	$Environments = $true
}

$kMcGuireUrl = 'https://casual-effects.com/g3d/data10'
$kGltfRepository = 'https://github.com/KhronosGroup/glTF-Sample-Assets.git'
$kGltfCommit = 'edc7c9e67c639d230715049ee31f9a96a6babbbe'
$kEnvironmentsUrl = 'https://media.githubusercontent.com/media/KhronosGroup/glTF-Sample-Environments'
$kEnvironmentsCommit = '4fc29557763275b8519b42920f85ad0ca1822f22'

# messages go to stderr: stdout is the list of paths
function Write-Message([string] $Message) { [Console]::Error.WriteLine($Message) }

function Get-Sha256([string] $Path) { (Get-FileHash -Algorithm SHA256 -LiteralPath $Path).Hash.ToLowerInvariant() }

Add-Type -AssemblyName System.IO.Compression.FileSystem

$failed = $false
$paths = [System.Collections.Generic.List[string]]::new()

if ($McGuire)
{
	$out = Join-Path $Dir 'mcguire'
	New-Item -ItemType Directory -Force -Path $out | Out-Null

	# <path in the archive> <local name> <size> <sha256>
	foreach ($line in Get-Content (Join-Path $root 'scripts/test-assets/mcguire.txt'))
	{
		if ([string]::IsNullOrWhiteSpace($line) -or $line.StartsWith('#'))
		{
			continue
		}
		$source, $name, $size, $hash = $line -split '\s+'
		$file = Join-Path $out $name

		if ((Test-Path -LiteralPath $file -PathType Leaf) -and (Get-Item -LiteralPath $file).Length -eq [int64]$size -and
			(Get-Sha256 $file) -eq $hash)
		{
			continue
		}

		Write-Message "fetching $name ($([int64]$size / 1MB -as [int]) MiB)"
		New-Item -ItemType Directory -Force -Path (Split-Path $file) | Out-Null
		$part = "$file.part"
		try
		{
			Invoke-WebRequest -Uri "$kMcGuireUrl/$source" -OutFile $part -MaximumRetryCount 3 -RetryIntervalSec 5
		}
		catch
		{
			Write-Message "failed to download $kMcGuireUrl/${source}: $($_.Exception.Message)"
			Remove-Item -LiteralPath $part -Force -ErrorAction SilentlyContinue
			$failed = $true
			continue
		}

		$actual = Get-Sha256 $part
		if ($actual -ne $hash)
		{
			Move-Item -LiteralPath $part -Destination "$file.unverified" -Force
			Write-Message "$name changed upstream (sha256 $actual, expected $hash): kept as $file.unverified."
			Write-Message '  check it, then update scripts/test-assets/mcguire.txt'
			$failed = $true
			continue
		}
		Move-Item -LiteralPath $part -Destination $file -Force
	}

	# Bistro comes in parts that reference each other (Exterior/exterior.mtl names ..\BuildingTextures\...), so each is
	# extracted into a directory of its name, side by side
	$bistro = Join-Path $out 'bistro'
	$parts = @(Get-ChildItem -Path (Join-Path $out 'bistro-parts') -Filter '*.zip' -File -ErrorAction SilentlyContinue)
	if (-not (Test-Path (Join-Path $bistro '.extracted')) -and $parts.Count -gt 0)
	{
		Write-Message 'extracting bistro'
		Remove-Item -LiteralPath $bistro -Recurse -Force -ErrorAction SilentlyContinue
		New-Item -ItemType Directory -Force -Path $bistro | Out-Null
		$extracted = $true
		foreach ($part in $parts)
		{
			try
			{
				[System.IO.Compression.ZipFile]::ExtractToDirectory($part.FullName, (Join-Path $bistro $part.BaseName), $true)
			}
			catch
			{
				Write-Message "failed to extract $($part.FullName): $($_.Exception.Message)"
				$extracted = $false
				$failed = $true
			}
		}
		if ($extracted)
		{
			New-Item -ItemType File -Force -Path (Join-Path $bistro '.extracted') | Out-Null
		}
	}

	# sorted by their bytes, as the shell's glob did, rather than by culture
	$zips = [string[]]@(Get-ChildItem -Path $out -Filter '*.zip' -File | ForEach-Object FullName)
	[Array]::Sort($zips, [StringComparer]::Ordinal)
	$zips | ForEach-Object { $paths.Add($_) }
	if (Test-Path (Join-Path $bistro '.extracted'))
	{
		$paths.Add($bistro)
	}
}

if ($Gltf)
{
	$out = Join-Path $Dir 'glTF-Sample-Assets'
	$head = if (Test-Path (Join-Path $out '.git')) { git -C $out rev-parse HEAD 2>$null } else { $null }
	if ($head -ne $kGltfCommit)
	{
		Write-Message "fetching glTF-Sample-Assets at $kGltfCommit"
		New-Item -ItemType Directory -Force -Path $out | Out-Null
		$steps = @(
			{ git -C $out init -q },
			{ git -C $out remote get-url origin 2>$null | Out-Null; if ($LASTEXITCODE -ne 0) { git -C $out remote add origin $kGltfRepository } },
			{ git -C $out sparse-checkout set Models },
			{ git -C $out fetch -q --depth 1 --filter=blob:none origin $kGltfCommit },
			{ git -C $out checkout -q --detach FETCH_HEAD })
		foreach ($step in $steps)
		{
			& $step | ForEach-Object { Write-Message $_ }
			if ($LASTEXITCODE -ne 0)
			{
				Write-Message 'failed to fetch glTF-Sample-Assets'
				$failed = $true
				break
			}
		}
	}
	if (Test-Path (Join-Path $out 'Models') -PathType Container)
	{
		$paths.Add((Join-Path $out 'Models'))
	}
}

if ($Environments)
{
	$out = Join-Path $Dir 'glTF-Sample-Environments'
	New-Item -ItemType Directory -Force -Path $out | Out-Null

	# <file> <size> <sha256>
	$complete = $true
	foreach ($line in Get-Content (Join-Path $root 'scripts/test-assets/environments.txt'))
	{
		if ([string]::IsNullOrWhiteSpace($line) -or $line.StartsWith('#'))
		{
			continue
		}
		$name, $size, $hash = $line -split '\s+'
		$file = Join-Path $out $name

		if ((Test-Path -LiteralPath $file -PathType Leaf) -and (Get-Item -LiteralPath $file).Length -eq [int64]$size -and
			(Get-Sha256 $file) -eq $hash)
		{
			continue
		}

		Write-Message "fetching $name ($([int64]$size / 1MB -as [int]) MiB)"
		$part = "$file.part"
		try
		{
			Invoke-WebRequest -Uri "$kEnvironmentsUrl/$kEnvironmentsCommit/$name" -OutFile $part -MaximumRetryCount 3 -RetryIntervalSec 5
		}
		catch
		{
			Write-Message "failed to download ${name}: $($_.Exception.Message)"
			Remove-Item -LiteralPath $part -Force -ErrorAction SilentlyContinue
			$failed = $true
			$complete = $false
			continue
		}

		$actual = Get-Sha256 $part
		if ($actual -ne $hash)
		{
			Remove-Item -LiteralPath $part -Force
			Write-Message "$name doesn't match its LFS pointer (sha256 $actual, expected $hash)"
			$failed = $true
			$complete = $false
			continue
		}
		Move-Item -LiteralPath $part -Destination $file -Force
	}
	if ($complete)
	{
		$paths.Add($out)
	}
}

$paths.Add((Join-Path $root 'scripts/test-assets/gltf'))
$paths.Add((Join-Path $root 'scripts/test-assets/obj'))

$paths | Write-Output
exit [int]$failed
