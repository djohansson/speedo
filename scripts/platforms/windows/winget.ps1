# Uses the winget CLI (ships with Windows 10 1809+/11 as part of App Installer) rather than the
# Microsoft.WinGet.Client module: modules install into the user's Documents folder, which may be
# redirected to OneDrive (cloud-only files there are skipped by module auto-loading).
function Assert-WinGet
{
	if (-not (Get-Command "winget" -ErrorAction SilentlyContinue))
	{
		Write-Error "winget not found. Install or update 'App Installer' from the Microsoft Store: https://aka.ms/getwinget"
	}
}

function Invoke-WinGetInstall
{
	param(
		[Parameter(Mandatory = $True)] [string] $Id,
		[Parameter(Mandatory = $False)] [string] $Override
	)

	$WinGetArgs = @("install", "--exact", "--silent", "--id", $Id, "--source", "winget", "--accept-package-agreements", "--accept-source-agreements", "--disable-interactivity")
	if ($Override)
	{
		# --force is required to circumvent the fact that the package could already be installed with a different configuration
		$WinGetArgs += @("--override", $Override, "--force")
	}

	& winget @WinGetArgs | Out-Host
	return $LASTEXITCODE
}
