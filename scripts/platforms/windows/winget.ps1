function Install-WinGetClient
{
	if (-not (Get-InstalledModule Microsoft.WinGet.Client -ErrorAction SilentlyContinue))
	{
		Write-Host "Installing Microsoft.WinGet.Client package..."
	
		Install-Package Microsoft.WinGet.Client -Scope CurrentUser -Force -Confirm:$False | Out-Null
	}
}
