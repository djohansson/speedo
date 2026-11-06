# Uploads this machine's vcpkg packages to the shared binary cache (see scripts/binarycache.ps1, docs/notes/build.md): the
# archives of what install/ holds now (install/vcpkg/status's ABIs), from vcpkg's local cache, those the bucket doesn't
# have yet. vcpkg uploads what it builds where SPEEDO_BINARY_CACHE_WRITE is set, but not what it restored from the local
# cache, so a machine whose packages were built before writing was set up seeds the bucket with this.
#
# Needs the AWS CLI (brew install awscli) and a B2 application key that can write to the bucket, in the AWS CLI's own
# credentials (AWS_ACCESS_KEY_ID and AWS_SECRET_ACCESS_KEY, or AWS_PROFILE). -DryRun lists what would be uploaded.
param(
	[switch] $DryRun,
	[string] $InstallRoot = (Join-Path $PSScriptRoot '../install')
)
$ErrorActionPreference = 'Stop'
. $PSScriptRoot/binarycache.ps1

$StatusFile = Join-Path $InstallRoot 'vcpkg/status'
if (-not (Test-Path $StatusFile))
{
	Write-Error "No vcpkg status at ${StatusFile}: run setup first"
}
if (-not $DryRun -and -not (Get-Command aws -ErrorAction SilentlyContinue))
{
	Write-Error 'The AWS CLI is not on the path (brew install awscli on macOS)'
}

# the installed packages: the status file's paragraphs with an ABI (a package, not a feature) that are installed
$Packages = @()
foreach ($Paragraph in ((Get-Content -Path $StatusFile -Raw) -split "\r?\n\r?\n"))
{
	$Fields = @{}
	foreach ($Line in ($Paragraph -split "\r?\n"))
	{
		if ($Line -match '^([A-Za-z-]+):\s*(.*)$') { $Fields[$Matches[1]] = $Matches[2] }
	}
	if ($Fields['Abi'] -and $Fields['Status'] -match 'installed$')
	{
		$Packages += [pscustomobject] @{ Name = $Fields['Package']; Triplet = $Fields['Architecture']; Abi = $Fields['Abi'] }
	}
}

$LocalCache = Get-LocalBinaryCachePath
foreach ($Entry in (Get-BinaryCacheAwsEnvironment).GetEnumerator())
{
	[Environment]::SetEnvironmentVariable($Entry.Key, $Entry.Value)
}

$Uploaded = 0
$UploadedBytes = 0
$Present = 0
$Missing = 0
foreach ($Package in ($Packages | Sort-Object Triplet, Name))
{
	$Label = "$($Package.Name):$($Package.Triplet)"
	$Archive = Join-Path $LocalCache "$($Package.Abi.Substring(0, 2))/$($Package.Abi).zip"
	if (-not (Test-Path $Archive))
	{
		Write-Warning "$Label has no local archive ($Archive)"
		$Missing++
		continue
	}
	$Url = Get-BinaryCacheUrl $Package.Abi
	$Head = Invoke-WebRequest -Method Head -Uri $Url -SkipHttpErrorCheck
	if ($Head.StatusCode -eq 200)
	{
		$Present++
		continue
	}
	$Bytes = (Get-Item $Archive).Length
	if ($DryRun)
	{
		Write-Host ('would upload {0} ({1:N1} MB)' -f $Label, ($Bytes / 1MB))
	}
	else
	{
		Write-Host ('uploading {0} ({1:N1} MB)' -f $Label, ($Bytes / 1MB))
		aws s3 cp --only-show-errors $Archive "s3://$BinaryCacheBucket/$BinaryCachePrefix$($Package.Abi).zip"
		if ($LASTEXITCODE -ne 0)
		{
			Write-Error "aws s3 cp failed for $Label (exit code $LASTEXITCODE)"
		}
		# readable anonymously, as vcpkg's http source reads it
		if ((Invoke-WebRequest -Method Head -Uri $Url -SkipHttpErrorCheck).StatusCode -ne 200)
		{
			Write-Error "$Label was uploaded but $Url doesn't answer (is the bucket public?)"
		}
	}
	$Uploaded++
	$UploadedBytes += $Bytes
}

Write-Host ('{0} packages: {1} {2} ({3:N2} GB), {4} already in the bucket, {5} without a local archive' -f `
	$Packages.Count, $Uploaded, ($DryRun ? 'to upload' : 'uploaded'), ($UploadedBytes / 1GB), $Present, $Missing)
