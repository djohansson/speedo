# Deletes the shared binary cache's archives that no platform installs any more (see scripts/binarycache.ps1,
# docs/notes/build.md): what a triplet, toolchain or port change superseded, and what a run that failed partway uploaded.
# An archive is kept if any of the -Keep lists names its ABI: vcpkg status files (install/vcpkg/status, its installed
# packages) or plain lists of ABIs, one a line, which -WriteKeepList writes for a machine whose status file isn't at
# hand. Give every platform's list: an archive no list names is deleted, whichever platform it is for.
#
# Needs the AWS CLI and a B2 application key that can delete from the bucket, as seed-binary-cache.ps1 does. -DryRun lists
# what would be deleted. More than half of the bucket is refused without -Force: the likeliest cause is a platform's list
# left out. -MinAgeDays keeps what was uploaded more recently whatever the lists say: CI prunes by master's lists after
# each push (.github/workflows/ci.yml), and a maintainer's packages for a change not pushed yet (a port's, the toolchain's)
# would otherwise go, and a hosted runner build them again (LLVM, hours) once it is.
#
#   pwsh scripts/prune-binary-cache.ps1 -WriteKeepList ~/mac-abis.txt     (on each other machine)
#   pwsh scripts/prune-binary-cache.ps1 -Keep install/vcpkg/status,mac-abis.txt,linux-abis.txt -DryRun
#
# Then FASTBuild's cache (its own prefix, see binarycache.ps1), by age rather than lists: an object last dated more than
# -FastbuildMaxAgeDays ago ($FastbuildCacheExpiryDays by default) goes, every version of it. The plugin dates anew what it
# retrieves once it is older than half that (a copy onto itself), so what builds keep using stays; each copy leaves the
# version before it, deleted here too, as are delete markers (B2 keeps a file's versions until they are deleted by id).
# -FastbuildOnly prunes FASTBuild's cache alone (no lists needed).
param(
	[string[]] $Keep,
	[string] $WriteKeepList,
	[switch] $DryRun,
	[switch] $Force,
	[double] $MinAgeDays = 0,
	[double] $FastbuildMaxAgeDays = -1,
	[switch] $FastbuildOnly,
	[string] $InstallRoot = (Join-Path $PSScriptRoot '../install')
)
$ErrorActionPreference = 'Stop'
. $PSScriptRoot/binarycache.ps1
if ($FastbuildMaxAgeDays -lt 0) { $FastbuildMaxAgeDays = $FastbuildCacheExpiryDays }

# deletes the objects (a Key each, and a VersionId to delete that version for good), up to 1000 a request; returns how many
# failed
function Remove-BucketObjects
{
	param([object[]] $Objects)

	$Request = Join-Path ([IO.Path]::GetTempPath()) "prune-binary-cache-$PID.json"
	$Failed = 0
	try
	{
		for ($First = 0; $First -lt $Objects.Count; $First += 1000)
		{
			$Batch = $Objects[$First..([Math]::Min($First + 999, $Objects.Count - 1))]
			$Keys = @($Batch | ForEach-Object { if ($_.VersionId) { @{ Key = $_.Key; VersionId = $_.VersionId } } else { @{ Key = $_.Key } } })
			$Body = @{ Objects = $Keys; Quiet = $True } | ConvertTo-Json -Depth 3 -Compress
			[IO.File]::WriteAllText($Request, $Body)
			$Result = aws s3api delete-objects --bucket $BinaryCacheBucket --delete "file://$Request" --output json
			if ($LASTEXITCODE -ne 0) { Write-Error "delete-objects failed (exit code $LASTEXITCODE)" }
			$Failures = @(($Result | ConvertFrom-Json).Errors | Where-Object { $_ })
			foreach ($Failure in $Failures) { Write-Warning "$($Failure.Key): $($Failure.Code) $($Failure.Message)" }
			$Failed += $Failures.Count
		}
	}
	finally
	{
		Remove-Item -Force -ErrorAction SilentlyContinue $Request
	}
	$Failed
}

# the ABIs of a vcpkg status file's installed packages (a package, not a feature, has an ABI)
function Get-InstalledAbis
{
	param([Parameter(Mandatory = $True)] [string] $StatusFile)

	foreach ($Paragraph in ((Get-Content -Path $StatusFile -Raw) -split "\r?\n\r?\n"))
	{
		# "install ok installed": a removed package's paragraph stays, "purge ok not-installed", which ends in "installed" too
		if ($Paragraph -match '(?m)^Status:\s*install\s+ok\s+installed\s*$' -and $Paragraph -match '(?m)^Abi:\s*([0-9a-f]{64})\s*$')
		{
			$Matches[1] # the last match's: the ABI's
		}
	}
}

if ($WriteKeepList)
{
	$StatusFile = Join-Path $InstallRoot 'vcpkg/status'
	if (-not (Test-Path $StatusFile)) { Write-Error "No vcpkg status at ${StatusFile}: run setup first" }
	$Abis = @(Get-InstalledAbis $StatusFile)
	Set-Content -Path $WriteKeepList -Value $Abis
	Write-Host "$($Abis.Count) installed packages' ABIs written to $WriteKeepList"
	exit 0
}

if (-not $Keep -and -not $FastbuildOnly) { Write-Error 'Name every platform''s list with -Keep (status files or ABI lists)' }
if (-not (Get-Command aws -ErrorAction SilentlyContinue))
{
	Write-Error 'The AWS CLI is not on the path (setup installs it where SPEEDO_BINARY_CACHE_WRITE is set)'
}

foreach ($Entry in (Get-BinaryCacheAwsEnvironment).GetEnumerator())
{
	[Environment]::SetEnvironmentVariable($Entry.Key, $Entry.Value)
}
$Failed = 0

if (-not $FastbuildOnly)
{
# what to keep: a status file's installed packages, or a list's ABIs
$Kept = @{}
foreach ($List in ($Keep -split ',' | Where-Object { $_ })) # pwsh -File passes -Keep a,b as one string
{
	if (-not (Test-Path $List)) { Write-Error "No such list: $List" }
	$Text = Get-Content -Path $List -Raw
	$Abis = @(($Text -match '(?m)^Abi:') ? (Get-InstalledAbis $List) : ($Text -split '\s+' | Where-Object { $_ -match '^[0-9a-f]{64}$' }))
	if ($Abis.Count -eq 0) { Write-Error "$List names no ABI" }
	foreach ($Abi in $Abis) { $Kept[$Abi] = $List }
	Write-Host "$List`: $($Abis.Count) packages"
}

$Listing = aws s3api list-objects-v2 --bucket $BinaryCacheBucket --prefix $BinaryCachePrefix --query 'Contents[].[Key,LastModified,Size]' --output json
if ($LASTEXITCODE -ne 0) { Write-Error "Listing the bucket failed (exit code $LASTEXITCODE)" }
$Objects = @($Listing | ConvertFrom-Json | ForEach-Object {
	[pscustomobject] @{
		Key = $_[0]
		Abi = $_[0].Substring($BinaryCachePrefix.Length) -replace '\.zip$', ''
		Modified = ([datetime] $_[1]).ToUniversalTime()
		Size = [long] $_[2]
	}
})

$InBucket = @{}
foreach ($Object in $Objects) { $InBucket[$Object.Abi] = $True }
$Missing = @($Kept.Keys | Where-Object { -not $InBucket.ContainsKey($_) })
if ($Missing.Count -gt 0)
{
	Write-Warning "$($Missing.Count) kept packages aren't in the bucket (seed them: seed-binary-cache.ps1)"
}

$Unlisted = @($Objects | Where-Object { -not $Kept.ContainsKey($_.Abi) })
$Cutoff = (Get-Date).ToUniversalTime().AddDays(-$MinAgeDays)
$Stale = @($Unlisted | Where-Object { $_.Modified -lt $Cutoff } | Sort-Object Modified)
$StaleBytes = ($Stale | Measure-Object Size -Sum).Sum
Write-Host ('{0} archives in the bucket, {1} kept, {2} stale ({3:N2} GB)' -f $Objects.Count, ($Objects.Count - $Stale.Count), $Stale.Count, ($StaleBytes / 1GB))
if ($Unlisted.Count -gt $Stale.Count)
{
	Write-Host ('  {0} no list names kept as uploaded in the last {1} days' -f ($Unlisted.Count - $Stale.Count), $MinAgeDays)
}
foreach ($Day in ($Stale | Group-Object { $_.Modified.ToString('yyyy-MM-dd') }))
{
	Write-Host ('  uploaded {0}: {1} ({2:N0} MB)' -f $Day.Name, $Day.Count, (($Day.Group | Measure-Object Size -Sum).Sum / 1MB))
}

if ($Stale.Count -gt 0 -and $DryRun)
{
	foreach ($Object in $Stale) { Write-Host ('would delete {0} ({1:N1} MB, {2:u})' -f $Object.Key, ($Object.Size / 1MB), $Object.Modified) }
}
elseif ($Stale.Count -gt 0)
{
	if ($Stale.Count * 2 -gt $Objects.Count -and -not $Force)
	{
		Write-Error "$($Stale.Count) of $($Objects.Count) archives would be deleted: is a platform's list missing? -Force deletes them"
	}
	$ArchivesFailed = Remove-BucketObjects $Stale
	Write-Host ('{0} deleted ({1:N2} GB), {2} failed' -f ($Stale.Count - $ArchivesFailed), ($StaleBytes / 1GB), $ArchivesFailed)
	$Failed += $ArchivesFailed
	# B2 keeps a deleted file's versions unless the bucket's lifecycle keeps only the last
}
}

# FASTBuild's cache: every version of each object, and its delete markers
$Listing = aws s3api list-object-versions --bucket $BinaryCacheBucket --prefix $FastbuildCachePrefix --output json
if ($LASTEXITCODE -ne 0) { Write-Error "Listing FASTBuild's cache failed (exit code $LASTEXITCODE)" }
$Parsed = $Listing | ConvertFrom-Json
$Versions = @($Parsed.Versions | Where-Object { $_ } | ForEach-Object {
	[pscustomobject] @{
		Key = $_.Key
		VersionId = $_.VersionId
		Latest = [bool] $_.IsLatest
		Modified = ([datetime] $_.LastModified).ToUniversalTime()
		Size = [long] $_.Size
	}
})
$Markers = @($Parsed.DeleteMarkers | Where-Object { $_ } | ForEach-Object {
	[pscustomobject] @{ Key = $_.Key; VersionId = $_.VersionId; Latest = [bool] $_.IsLatest; Modified = [datetime]::MinValue; Size = 0L }
})
# an object is kept while its latest version is younger than the limit: then only its older versions and its markers go
$FastbuildCutoff = (Get-Date).ToUniversalTime().AddDays(-$FastbuildMaxAgeDays)
$LiveKeys = @{}
foreach ($Version in $Versions) { if ($Version.Latest -and $Version.Modified -ge $FastbuildCutoff) { $LiveKeys[$Version.Key] = $True } }
$Expired = @($Versions + $Markers | Where-Object { -not $LiveKeys.ContainsKey($_.Key) })
$Superseded = @($Versions + $Markers | Where-Object { $LiveKeys.ContainsKey($_.Key) -and -not $_.Latest })
$Doomed = @($Expired + $Superseded)
$DoomedBytes = ($Doomed | Measure-Object Size -Sum).Sum
Write-Host ('FASTBuild''s cache: {0} objects kept (younger than {1} days), {2} versions of {3} expired ones and {4} superseded ones to delete ({5:N2} GB)' -f
	$LiveKeys.Count, $FastbuildMaxAgeDays, $Expired.Count, @($Expired | Select-Object -Unique -Property Key).Count, $Superseded.Count, ($DoomedBytes / 1GB))
if ($Doomed.Count -gt 0 -and $DryRun)
{
	foreach ($Object in $Doomed) { Write-Host ('would delete {0} version {1} ({2:N1} MB, {3:u})' -f $Object.Key, $Object.VersionId, ($Object.Size / 1MB), $Object.Modified) }
}
elseif ($Doomed.Count -gt 0)
{
	$ObjectsFailed = Remove-BucketObjects $Doomed
	Write-Host ('{0} deleted ({1:N2} GB), {2} failed' -f ($Doomed.Count - $ObjectsFailed), ($DoomedBytes / 1GB), $ObjectsFailed)
	$Failed += $ObjectsFailed
}

if ($Failed -gt 0) { exit 1 }
