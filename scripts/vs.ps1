function Install-VSSetup
{
	if (-not (Get-InstalledModule VSSetup -ErrorAction SilentlyContinue))
	{
		Write-Host "Installing VSSetup module..."

		Install-Module VSSetup -Scope CurrentUser -Confirm:$False -Force
	}
}