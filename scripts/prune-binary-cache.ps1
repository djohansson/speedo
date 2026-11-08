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
param(
	[string[]] $Keep,
	[string] $WriteKeepList,
	[switch] $DryRun,
	[switch] $Force,
	[double] $MinAgeDays = 0,
	[string] $InstallRoot = (Join-Path $PSScriptRoot '../install')
)
$ErrorActionPreference = 'Stop'
. $PSScriptRoot/binarycache.ps1

# the ABIs of a vcpkg status file's installed packages (a package, not a feature, has an ABI)
function Get-InstalledAbis
{
	param([Parameter(Mandatory = $True)] [string] $StatusFile)

	foreach ($Paragraph in ((Get-Content -Path $StatusFile -Raw) -split "\r?\n\r?\n"))
	{
		if ($Paragraph -match '(?m)^Status:.*installed\s*$' -and $Paragraph -match '(?m)^Abi:\s*([0-9a-f]{64})\s*$')
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

if (-not $Keep) { Write-Error 'Name every platform''s list with -Keep (status files or ABI lists)' }
if (-not (Get-Command aws -ErrorAction SilentlyContinue))
{
	Write-Error 'The AWS CLI is not on the path (setup installs it where SPEEDO_BINARY_CACHE_WRITE is set)'
}

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

foreach ($Entry in (Get-BinaryCacheAwsEnvironment).GetEnumerator())
{
	[Environment]::SetEnvironmentVariable($Entry.Key, $Entry.Value)
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

if ($Stale.Count -eq 0) { exit 0 }
if ($DryRun)
{
	foreach ($Object in $Stale) { Write-Host ('would delete {0} ({1:N1} MB, {2:u})' -f $Object.Key, ($Object.Size / 1MB), $Object.Modified) }
	exit 0
}
if ($Stale.Count * 2 -gt $Objects.Count -and -not $Force)
{
	Write-Error "$($Stale.Count) of $($Objects.Count) archives would be deleted: is a platform's list missing? -Force deletes them"
}

# delete-objects takes up to 1000 keys a request
$Request = Join-Path ([IO.Path]::GetTempPath()) "prune-binary-cache-$PID.json"
$Failed = 0
try
{
	for ($First = 0; $First -lt $Stale.Count; $First += 1000)
	{
		$Batch = $Stale[$First..([Math]::Min($First + 999, $Stale.Count - 1))]
		$Body = @{ Objects = @($Batch | ForEach-Object { @{ Key = $_.Key } }); Quiet = $True } | ConvertTo-Json -Depth 3 -Compress
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

Write-Host ('{0} deleted ({1:N2} GB), {2} failed' -f ($Stale.Count - $Failed), ($StaleBytes / 1GB), $Failed)
if ($Failed -gt 0) { exit 1 }
# B2 keeps a deleted file's versions unless the bucket's lifecycle keeps only the last
