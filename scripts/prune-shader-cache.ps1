# Prunes a build's shader cache (build/<preset>/shader-cache, see docs/shader-compile-plan.md, "The cache"): what a client
# hasn't used for -MaxAgeDays, then the least recently used until the rest fit in -MaxSizeGB. The cache holds each entry
# point's compiled code, named by a hash of its source, slang's version and the configuration, and round one's keys records;
# the client removes none, so every shader edit adds what it changed (a developer's grew to 17.6 GB in six days,
# 2026-11-24). It dates a file when it takes one (gfx/shaderloader.cpp's MarkUsed), so a file's date is its last use. What
# goes is compiled again where it is next needed: a layout's cold compile, 40 s to 2 minutes.
#
# Also what a client killed mid-compile leaves, over an hour old: a file written beside its name (<name>.<16 hex digits>)
# and the workers' job directories (jobs/<id>). The costs files (costs-<format>: what each job took, by which the workers'
# shares are made) stay, as does anything else. A running client is safe: one that looks for a file removed meanwhile
# compiles it again.
#
#   pwsh scripts/prune-shader-cache.ps1 -DryRun                           (every preset's, what would go)
#   pwsh scripts/prune-shader-cache.ps1 -Preset x64-windows-clang-profile -MaxSizeGB 1
#   pwsh scripts/prune-shader-cache.ps1 -Path build/x64-linux-clang-profile/shader-cache   (CI's, before it is kept)
param(
	[string[]] $Preset,
	[string[]] $Path,
	[double] $MaxSizeGB = 2,
	[double] $MaxAgeDays = 14,
	[switch] $DryRun
)
$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$directories = @(
	if ($Path) { $Path | ForEach-Object { if ([IO.Path]::IsPathRooted($_)) { $_ } else { Join-Path (Get-Location) $_ } } }
	if ($Preset) { $Preset | ForEach-Object { Join-Path $root "build/$_/shader-cache" } }
	if (-not $Path -and -not $Preset) { Get-ChildItem -Path (Join-Path $root 'build/*/shader-cache') -Directory -ErrorAction SilentlyContinue | ForEach-Object FullName }
)
if ($directories.Count -eq 0)
{
	Write-Host 'no shader cache to prune'
	exit 0
}

$now = Get-Date
$maxBytes = [long]($MaxSizeGB * 1GB)
$oldest = $now.AddDays(-$MaxAgeDays)
$abandoned = $now.AddHours(-1)
# an entry (<sha256>.bin), a keys record (keys-<sha256>.bin) or a record's dependency list (keys-<sha256>.deps)
$cachedName = '^(keys-)?[0-9a-f]{64}\.(bin|deps)$'
# a file written beside its name, to be renamed into place
$temporaryName = '\.[0-9a-f]{16}$'
foreach ($directory in $directories)
{
	if (-not (Test-Path -LiteralPath $directory -PathType Container))
	{
		Write-Host "${directory}: not there"
		continue
	}
	$files = @(Get-ChildItem -LiteralPath $directory -File)
	$totalBytes = ($files | Measure-Object Length -Sum).Sum
	# the most recently used first: kept while they are recent enough and fit
	$cached = @($files | Where-Object Name -Match $cachedName | Sort-Object LastWriteTime -Descending)
	$keptBytes = [long]0
	$keptCount = 0
	$keptOldest = $null
	$stale = [Collections.Generic.List[IO.FileInfo]]::new()
	$over = [Collections.Generic.List[IO.FileInfo]]::new()
	# (the first that doesn't fit, and all used before it: not a smaller one used earlier in its place)
	$full = $false
	foreach ($file in $cached)
	{
		if ($file.LastWriteTime -lt $oldest) { $stale.Add($file) }
		elseif ($full -or $keptBytes + $file.Length -gt $maxBytes) { $over.Add($file); $full = $true }
		else { $keptBytes += $file.Length; $keptCount++; $keptOldest = $file.LastWriteTime }
	}
	$temporaries = @($files | Where-Object { $_.Name -match $temporaryName -and $_.LastWriteTime -lt $abandoned })
	$jobsDirectory = Join-Path $directory 'jobs'
	$jobs = @(if (Test-Path -LiteralPath $jobsDirectory) { Get-ChildItem -LiteralPath $jobsDirectory -Directory | Where-Object LastWriteTime -LT $abandoned })

	$removed = @($stale) + @($over) + $temporaries
	$removedBytes = ($removed | Measure-Object Length -Sum).Sum
	$failed = 0
	if (-not $DryRun)
	{
		foreach ($file in $removed)
		{
			# (a client's open file, on Windows: left for the next time)
			try { [IO.File]::Delete($file.FullName) } catch { $failed++ }
		}
		foreach ($job in $jobs)
		{
			try { Remove-Item -LiteralPath $job.FullName -Recurse -Force } catch { $failed++ }
		}
	}
	$verb = if ($DryRun) { 'would remove' } else { 'removed' }
	$since = if ($keptOldest) { ', the oldest used {0:yyyy-MM-dd HH:mm}' -f $keptOldest } else { '' }
	Write-Host ("{0}: {1} files, {2:N0} MB; {3} {4} ({5:N0} MB: {6} unused for over {7} days, {8} past {9:0.###} GB, {10} left by a write) and {11} job directories; kept {12} ({13:N0} MB{14})" -f
		$directory, $files.Count, ($totalBytes / 1MB), $verb, $removed.Count, ($removedBytes / 1MB), $stale.Count, $MaxAgeDays,
		$over.Count, $MaxSizeGB, $temporaries.Count, $jobs.Count, $keptCount, ($keptBytes / 1MB), $since)
	if ($failed -gt 0)
	{
		Write-Warning "${directory}: $failed couldn't be removed (a running client's, on Windows): left for the next time"
	}
}
