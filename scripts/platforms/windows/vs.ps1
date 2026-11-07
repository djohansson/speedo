# Uses vswhere.exe rather than the VSSetup module (see winget.ps1 for why modules are avoided).
# The Visual Studio Installer always places vswhere at this fixed location; if it is missing, no
# Visual Studio (or Build Tools) instance is installed.
#
# -RequireAny: one of these too (vswhere's -requiresAny applies to every -requires, so a query per alternative)
function Get-VSInstance
{
	param(
		[Parameter(Mandatory = $True)] [string[]] $Require,
		[Parameter(Mandatory = $False)] [string[]] $RequireAny = @()
	)

	$VSWhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
	if (-not (Test-Path $VSWhere))
	{
		return $null
	}

	$Queries = [System.Collections.Generic.List[object]]::new()
	if ($RequireAny.Count -eq 0) { $Queries.Add($Require) }
	foreach ($Any in $RequireAny) { $Queries.Add($Require + $Any) }

	# in vswhere's order, each instance once
	$Found = [ordered]@{}
	foreach ($Requirements in $Queries)
	{
		$Listed = & $VSWhere -products * -requires @Requirements -format json -utf8 | ConvertFrom-Json
		if ($LASTEXITCODE -ne 0) { Write-Error "vswhere failed with exit code $LASTEXITCODE" }
		foreach ($Instance in $Listed)
		{
			if (-not $Found.Contains($Instance.instanceId)) { $Found[$Instance.instanceId] = $Instance }
		}
	}
	$Instances = $Found.Values

	# -Stable keeps vswhere's order for equal versions: vcpkg only keeps one instance per version
	# (the first vswhere lists) and fails if VCPKG_VISUAL_STUDIO_PATH names the one it dropped
	return $Instances | Sort-Object { [System.Version]$_.installationVersion } -Descending -Stable | Select-Object -First 1
}
