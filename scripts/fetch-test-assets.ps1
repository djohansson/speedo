<#
.SYNOPSIS
Downloads the asset sets that scripts/assettest.ps1 tests the model and image loaders on.

.DESCRIPTION
Each set is a manifest in resources/test-asset-manifests: its directory under <dir>, the url its files are under, and each
file's size and sha256 (tab separated: <size> <sha256> <path>, and the path under the url where it differs). A file is
downloaded unless it is there with its size and sha256, several at once, and checked: one that doesn't match is reported
(and kept as <name>.unverified) rather than used.

-McGuire: Morgan McGuire's Computer Graphics Archive (https://casual-effects.com/data), obj models with their materials
and textures, as zip archives (about 2.7 GB), into <dir>/mcguire (mcguire.txt). The archive publishes no versions, so a
file that changed upstream is reported until the manifest is updated. Bistro's parts are extracted side by side into
<dir>/mcguire/bistro (the manifest's extract line).

-Gltf: the Khronos glTF-Sample-Assets models (https://github.com/KhronosGroup/glTF-Sample-Assets), at the commit gltf.txt
pins (about 1.4 GB, from raw.githubusercontent.com), into <dir>/glTF-Sample-Assets. scripts/update-gltf-manifest.ps1
writes the manifest for another commit.

-Environments: the Khronos glTF-Sample-Environments panoramas (https://github.com/KhronosGroup/glTF-Sample-Environments,
the .hdr files the glTF Sample Viewer lights with), at the commit environments.txt pins (about 400 MB), into
<dir>/glTF-Sample-Environments. They are Git LFS files, downloaded from GitHub's LFS media server, their sizes and sha256s
their LFS pointers'.

Without any of them, all are fetched. The hand-made models in resources/test-models/gltf and resources/test-models/obj (for
what no downloaded model covers, e.g. sparse index accessors, or bump map tangents) are always printed too. The dir defaults to $env:SPEEDO_TEST_ASSETS, or
resources/test-assets (which git ignores, and the client's file dialogs open in). Files already there (and verified)
are kept, so running it again only fetches what is missing.

-Quick prints only the quick tier's paths (resources/test-asset-manifests/quick.txt: a model or two per feature, for
iterating), after fetching the sets as above.
-Minimal fetches only what the quick tier names (its McGuire archives, glTF models and panoramas, a few hundred MB rather
than about 4.5 GB) and prints them as -Quick does: for CI (.github/workflows/ci.yml). It never narrows what is there.

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
	[switch] $Environments,
	[switch] $Quick,
	[switch] $Minimal
)

$ErrorActionPreference = 'Stop'
# Invoke-WebRequest's progress display slows large downloads down a lot
$ProgressPreference = 'SilentlyContinue'

$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$manifests = Join-Path $root 'resources/test-asset-manifests'
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

# the quick tier's entries (paths under the test asset dir: files, or directories of them), which -Minimal fetches alone
$quickEntries = @(Get-Content -Encoding utf8 (Join-Path $manifests 'quick.txt') | ForEach-Object { $_.Trim() } |
	Where-Object { $_ -and -not $_.StartsWith('#') })
if ($Minimal)
{
	$Quick = $true
}

# messages go to stderr: stdout is the list of paths
function Write-Message([string] $Message) { [Console]::Error.WriteLine($Message) }

# whether a path under the test asset dir is one of the quick tier's, or under one
function Test-QuickEntry([string] $Path)
{
	foreach ($entry in $quickEntries)
	{
		if ($Path -eq $entry -or $Path.StartsWith("$entry/", [StringComparison]::Ordinal))
		{
			return $true
		}
	}
	$false
}

# a manifest: its directory (dir), the url its files are under (url), the directories of zips extracted (extract <from>
# <into>), and its files (<size> <sha256> <path> [<path under the url>])
function Read-Manifest([string] $Name)
{
	$manifest = [pscustomobject]@{ Name = $Name; Dir = $null; Url = $null; Extract = @(); Entries = @() }
	$entries = [System.Collections.Generic.List[object]]::new()
	foreach ($line in Get-Content -Encoding utf8 -LiteralPath (Join-Path $manifests $Name))
	{
		if ([string]::IsNullOrWhiteSpace($line) -or $line.StartsWith('#'))
		{
			continue
		}
		$fields = $line -split "`t"
		switch ($fields[0])
		{
			'dir' { $manifest.Dir = $fields[1] }
			'url' { $manifest.Url = $fields[1] }
			'extract' { $manifest.Extract += , @($fields[1], $fields[2]) }
			default
			{
				$entries.Add([pscustomobject]@{
						Size = [int64]$fields[0]
						Hash = $fields[1]
						Path = $fields[2]
						Source = if ($fields.Count -gt 3) { $fields[3] } else { $fields[2] }
					})
			}
		}
	}
	$manifest.Entries = $entries.ToArray()
	$manifest
}

Add-Type -AssemblyName System.IO.Compression.FileSystem

# fetches a manifest's files (with -Minimal, those the quick tier names) that aren't in <dir>/<its dir> with their sizes
# and sha256s, 8 at once. returns whether all of them are there
function Sync-Manifest($Manifest)
{
	$out = Join-Path $Dir $Manifest.Dir
	$wanted = @($Manifest.Entries | Where-Object { -not $Minimal -or (Test-QuickEntry "$($Manifest.Dir)/$($_.Path)") })
	$url = $Manifest.Url
	$name = $Manifest.Name
	$results = @($wanted | ForEach-Object -ThrottleLimit 8 -Parallel {
			$entry = $_
			$file = Join-Path $using:out $entry.Path
			$hashOf = { param($path) (Get-FileHash -Algorithm SHA256 -LiteralPath $path).Hash.ToLowerInvariant() }
			if ((Test-Path -LiteralPath $file -PathType Leaf) -and (Get-Item -LiteralPath $file).Length -eq $entry.Size -and
				(& $hashOf $file) -eq $entry.Hash)
			{
				return $true
			}

			$ProgressPreference = 'SilentlyContinue'
			[Console]::Error.WriteLine("fetching $($entry.Path) ($([math]::Round($entry.Size / 1MB, 1)) MiB)")
			New-Item -ItemType Directory -Force -Path (Split-Path $file) | Out-Null
			$part = "$file.part"
			# each segment escaped: some paths have spaces, one UTF-8
			$source = "$using:url/" + (($entry.Source -split '/' | ForEach-Object { [Uri]::EscapeDataString($_) }) -join '/')
			try
			{
				Invoke-WebRequest -Uri $source -OutFile $part -MaximumRetryCount 3 -RetryIntervalSec 5
			}
			catch
			{
				[Console]::Error.WriteLine("failed to download ${source}: $($_.Exception.Message)")
				Remove-Item -LiteralPath $part -Force -ErrorAction SilentlyContinue
				return $false
			}
			$actual = & $hashOf $part
			$length = (Get-Item -LiteralPath $part).Length
			if ($actual -ne $entry.Hash -or $length -ne $entry.Size)
			{
				Move-Item -LiteralPath $part -Destination "$file.unverified" -Force
				[Console]::Error.WriteLine(
					"$($entry.Path) doesn't match the manifest ($length bytes, sha256 $actual; expected $($entry.Size), $($entry.Hash)): " +
					"kept as $file.unverified. check it, then update resources/test-asset-manifests/$using:name")
				return $false
			}
			Move-Item -LiteralPath $part -Destination $file -Force
			$true
		})
	$complete = $results -notcontains $false

	# zips extracted side by side, each into a directory of its name (Bistro's parts reference each other:
	# Exterior/exterior.mtl names ..\BuildingTextures\...), once all of them are there
	foreach ($extract in $Manifest.Extract)
	{
		$from, $into = $extract
		$parts = @($Manifest.Entries | Where-Object { $_.Path.StartsWith("$from/", [StringComparison]::Ordinal) })
		$target = Join-Path $out $into
		if ($parts.Count -eq 0 -or (Test-Path (Join-Path $target '.extracted')) -or
			@($parts | Where-Object { -not (Test-Path -LiteralPath (Join-Path $out $_.Path) -PathType Leaf) }).Count -gt 0)
		{
			continue
		}
		Write-Message "extracting $into"
		Remove-Item -LiteralPath $target -Recurse -Force -ErrorAction SilentlyContinue
		New-Item -ItemType Directory -Force -Path $target | Out-Null
		$extracted = $true
		foreach ($part in $parts)
		{
			$zip = Join-Path $out $part.Path
			try
			{
				[System.IO.Compression.ZipFile]::ExtractToDirectory($zip, (Join-Path $target ([System.IO.Path]::GetFileNameWithoutExtension($zip))), $true)
			}
			catch
			{
				Write-Message "failed to extract ${zip}: $($_.Exception.Message)"
				$extracted = $false
			}
		}
		if ($extracted)
		{
			New-Item -ItemType File -Force -Path (Join-Path $target '.extracted') | Out-Null
		}
		else
		{
			$complete = $false
		}
	}
	$complete
}

$failed = $false
$paths = [System.Collections.Generic.List[string]]::new()

if ($McGuire)
{
	$manifest = Read-Manifest 'mcguire.txt'
	$failed = -not (Sync-Manifest $manifest) -or $failed
	$out = Join-Path $Dir $manifest.Dir
	# sorted by their bytes, as the shell's glob did, rather than by culture
	$zips = [string[]]@(Get-ChildItem -Path $out -Filter '*.zip' -File -ErrorAction SilentlyContinue | ForEach-Object FullName)
	[Array]::Sort($zips, [StringComparer]::Ordinal)
	$zips | ForEach-Object { $paths.Add($_) }
	if (Test-Path (Join-Path $out 'bistro/.extracted'))
	{
		$paths.Add((Join-Path $out 'bistro'))
	}
}

if ($Gltf)
{
	$manifest = Read-Manifest 'gltf.txt'
	$failed = -not (Sync-Manifest $manifest) -or $failed
	$models = Join-Path (Join-Path $Dir $manifest.Dir) 'Models'
	if (Test-Path $models -PathType Container)
	{
		$paths.Add($models)
	}
}

if ($Environments)
{
	$manifest = Read-Manifest 'environments.txt'
	$complete = Sync-Manifest $manifest
	$failed = -not $complete -or $failed
	if ($complete)
	{
		$paths.Add((Join-Path $Dir $manifest.Dir))
	}
}

if ($Quick)
{
	$paths.Clear()
	foreach ($entry in $quickEntries)
	{
		$path = Join-Path $Dir $entry
		if (Test-Path -LiteralPath $path)
		{
			$paths.Add($path)
		}
		else
		{
			Write-Message "quick tier: $path is missing"
			$failed = $true
		}
	}
}
$paths.Add((Join-Path $root 'resources/test-models/gltf'))
$paths.Add((Join-Path $root 'resources/test-models/obj'))

$paths | Write-Output
exit [int]$failed
