<#
.SYNOPSIS
Writes resources/test-asset-manifests/gltf.txt: the Khronos glTF-Sample-Assets models' files at a commit, with their sizes
and sha256s.

.DESCRIPTION
For whoever moves the pinned commit: scripts/fetch-test-assets.ps1 and the client (File > Download Test Assets) download
the files the manifest names from raw.githubusercontent.com at its commit, each checked against its size and sha256.

-Checkout: a checkout of https://github.com/KhronosGroup/glTF-Sample-Assets with Models checked out, at the commit to pin
(its HEAD); without it, -Commit (by default the manifest's) is fetched into a temporary directory, Models alone (about
1.4 GB), and removed after.

.EXAMPLE
scripts/update-gltf-manifest.ps1 -Commit <sha>
#>
param(
	[string] $Checkout,
	[string] $Commit
)

$ErrorActionPreference = 'Stop'

$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$manifest = Join-Path $root 'resources/test-asset-manifests/gltf.txt'
$kRepository = 'https://github.com/KhronosGroup/glTF-Sample-Assets.git'
$kRawUrl = 'https://raw.githubusercontent.com/KhronosGroup/glTF-Sample-Assets'

function Write-Message([string] $Message) { [Console]::Error.WriteLine($Message) }

function Invoke-Git
{
	& git @args | ForEach-Object { Write-Message $_ }
	if ($LASTEXITCODE -ne 0)
	{
		throw "git $($args -join ' ') failed"
	}
}

$temporary = $null
try
{
	if (-not $Checkout)
	{
		if (-not $Commit -and (Test-Path $manifest))
		{
			$url = Get-Content -Encoding utf8 $manifest | Where-Object { $_.StartsWith("url`t") } | Select-Object -First 1
			if ($url) { $Commit = ($url -split '/')[-1] }
		}
		if (-not $Commit)
		{
			throw 'no -Commit given, and no manifest to take it from'
		}
		$temporary = Join-Path ([System.IO.Path]::GetTempPath()) "gltf-sample-assets-$([guid]::NewGuid())"
		Write-Message "fetching glTF-Sample-Assets' Models at $Commit into $temporary"
		New-Item -ItemType Directory -Path $temporary | Out-Null
		Invoke-Git -C $temporary init -q
		Invoke-Git -C $temporary remote add origin $kRepository
		Invoke-Git -C $temporary sparse-checkout set Models
		Invoke-Git -C $temporary fetch -q --depth 1 --filter=blob:none origin $Commit
		Invoke-Git -C $temporary checkout -q --detach FETCH_HEAD
		$Checkout = $temporary
	}

	$head = git -C $Checkout rev-parse HEAD
	if ($LASTEXITCODE -ne 0)
	{
		throw "$Checkout isn't a git checkout"
	}
	if ($Commit -and $head -ne $Commit)
	{
		throw "$Checkout is at $head, not $Commit"
	}

	# <mode> <type> <object> <size>\t<path>, the paths unquoted (some have spaces, one UTF-8)
	$lines = @(git -C $Checkout -c core.quotePath=false ls-tree -r --long HEAD Models)
	if ($LASTEXITCODE -ne 0 -or $lines.Count -eq 0)
	{
		throw "no Models in $Checkout at $head"
	}
	$entries = foreach ($line in $lines)
	{
		$meta, $path = $line -split "`t", 2
		$mode, $type, $object, $size = $meta -split '\s+'
		if ($type -ne 'blob')
		{
			throw "$path is a $type, not a file"
		}
		$file = Join-Path $Checkout $path
		# the bytes raw.githubusercontent.com serves are the blob's: a checkout that converted line endings would differ
		$length = (Get-Item -LiteralPath $file).Length
		if ($length -ne [int64]$size)
		{
			throw "$path is $length bytes in the checkout, $size in git: converted?"
		}
		[pscustomobject]@{ Path = $path; Size = [int64]$size; Hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $file).Hash.ToLowerInvariant() }
	}
	$sorted = [object[]]@($entries)
	[Array]::Sort($sorted, [Comparison[object]] { param($a, $b) [string]::CompareOrdinal($a.Path, $b.Path) })

	$text = [System.Text.StringBuilder]::new()
	[void]$text.AppendLine('# the Khronos glTF-Sample-Assets models (https://github.com/KhronosGroup/glTF-Sample-Assets) at the commit in the url, written')
	[void]$text.AppendLine('# by scripts/update-gltf-manifest.ps1: <size>, <sha256> and <path>, tab separated (the paths have spaces). each model''s')
	[void]$text.AppendLine('# license is its LICENSE.md')
	[void]$text.AppendLine("dir`tglTF-Sample-Assets")
	[void]$text.AppendLine("url`t$kRawUrl/$head")
	foreach ($entry in $sorted)
	{
		[void]$text.AppendLine("$($entry.Size)`t$($entry.Hash)`t$($entry.Path)")
	}
	[System.IO.File]::WriteAllText($manifest, $text.ToString().Replace("`r`n", "`n"), [System.Text.UTF8Encoding]::new($false))
	$total = ($sorted | Measure-Object -Property Size -Sum).Sum
	Write-Message "wrote $manifest`: $($sorted.Count) files, $([math]::Round($total / 1GB, 2)) GiB, at $head"
}
finally
{
	if ($temporary)
	{
		Remove-Item -LiteralPath $temporary -Recurse -Force -ErrorAction SilentlyContinue
	}
}
