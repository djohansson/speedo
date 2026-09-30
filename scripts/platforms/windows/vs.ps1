# Uses vswhere.exe rather than the VSSetup module (see winget.ps1 for why modules are avoided).
# The Visual Studio Installer always places vswhere at this fixed location; if it is missing, no
# Visual Studio (or Build Tools) instance is installed.
function Get-VSInstance
{
	param(
		[Parameter(Mandatory = $True)] [string[]] $Require
	)

	$VSWhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
	if (-not (Test-Path $VSWhere))
	{
		return $null
	}

	$Instances = & $VSWhere -products * -requires @Require -format json -utf8 | ConvertFrom-Json
	if ($LASTEXITCODE -ne 0) { Write-Error "vswhere failed with exit code $LASTEXITCODE" }

	# -Stable keeps vswhere's order for equal versions: vcpkg only keeps one instance per version
	# (the first vswhere lists) and fails if VCPKG_VISUAL_STUDIO_PATH names the one it dropped
	return $Instances | Sort-Object { [System.Version]$_.installationVersion } -Descending -Stable | Select-Object -First 1
}
