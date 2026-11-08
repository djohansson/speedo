<#
.SYNOPSIS
Bumps the version in src/version.h, commits it and tags the release, which .github/workflows/release.yml packages.

.DESCRIPTION
-Bump major|minor|patch (patch by default) raises that part of src/version.h's version and zeroes the parts after it;
-Version <major>.<minor>.<patch> sets it outright (newer than the current one). The working tree must be clean, on master,
and the tag v<version> new. It commits src/version.h ("release: v<version>") and tags that commit v<version> (annotated).

Nothing is pushed unless -Push: then master and the tag go to origin, and the tag starts the release workflow, which
checks it against src/version.h, makes the tag's draft release and attaches each platform's package and its sha256 (see
docs/notes/build.md's releases); the draft is published by hand once checked. Without -Push it prints the push command.
To undo an unpushed release: git tag -d v<version>, then git reset --hard HEAD~1.

.EXAMPLE
scripts/release.ps1 -Bump minor

.EXAMPLE
scripts/release.ps1 -Version 1.0.0 -Push
#>
param(
	[ValidateSet('major', 'minor', 'patch')]
	[string] $Bump = 'patch',
	[string] $Version,
	[switch] $Push
)

$ErrorActionPreference = 'Stop'

$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$headerPath = Join-Path $root 'src/version.h'

function Invoke-Git
{
	$output = & git -C $root @args
	if ($LASTEXITCODE -ne 0)
	{
		throw "git $($args -join ' ') failed ($LASTEXITCODE)"
	}
	$output
}

# the version's parts, as CMakeLists.txt and package.ps1 read them
$header = [System.IO.File]::ReadAllText($headerPath)
$parts = @('Major', 'Minor', 'Patch') | ForEach-Object { if ($header -match "$_ = ([0-9]+)") { [int]$Matches[1] } else { throw "no $_ version in src/version.h" } }
$current = [version]::new($parts[0], $parts[1], $parts[2])

if ($Version)
{
	if ($Version -notmatch '^([0-9]+)\.([0-9]+)\.([0-9]+)$')
	{
		throw "-Version takes <major>.<minor>.<patch>, got $Version"
	}
	$next = [version]::new([int]$Matches[1], [int]$Matches[2], [int]$Matches[3])
	if ($next -le $current)
	{
		throw "-Version $next isn't newer than src/version.h's $current"
	}
}
else
{
	$next = switch ($Bump)
	{
		'major' { [version]::new($current.Major + 1, 0, 0) }
		'minor' { [version]::new($current.Major, $current.Minor + 1, 0) }
		'patch' { [version]::new($current.Major, $current.Minor, $current.Build + 1) }
	}
}
$tag = "v$next"

if (Invoke-Git status --porcelain)
{
	throw 'the working tree has changes: commit or stash them first'
}
$branch = Invoke-Git rev-parse --abbrev-ref HEAD
if ($branch -ne 'master')
{
	throw "releases are tagged on master, not $branch"
}
& git -C $root rev-parse --quiet --verify "refs/tags/$tag" | Out-Null
if ($LASTEXITCODE -eq 0)
{
	throw "the tag $tag exists already"
}

$updated = $header
foreach ($part in @(@('Major', $next.Major), @('Minor', $next.Minor), @('Patch', $next.Build)))
{
	$updated = $updated -replace "($($part[0]) = )[0-9]+", "`${1}$($part[1])"
}
[System.IO.File]::WriteAllText($headerPath, $updated)

Invoke-Git add src/version.h | Out-Null
Invoke-Git commit --quiet -m "release: $tag" | Out-Null
Invoke-Git tag -a $tag -m "Speedo $next" | Out-Null
Write-Host "src/version.h $current -> $next, committed and tagged $tag"

if ($Push)
{
	Invoke-Git push origin master | Out-Null
	Invoke-Git push origin $tag | Out-Null
	Write-Host "pushed master and ${tag}: the release workflow makes the draft release"
}
else
{
	Write-Host "to release: git push origin master $tag (the tag starts the release workflow)"
}
