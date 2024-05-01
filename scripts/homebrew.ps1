function Install-HomebrewPackage
{
	param([Parameter(Mandatory = $True, Position = 0)] [string] $packageName)

	Write-Host "Installing $packageName..."
	if (brew list $packageName)
	{
		Write-Host "$packageName is already installed."
	}
	else
	{
		brew install $packageName
		Write-Host "$packageName is installed."
	}
}
