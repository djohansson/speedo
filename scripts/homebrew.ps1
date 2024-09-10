function Install-HomebrewPackage
{
	param([Parameter(Mandatory = $True, Position = 0)] [string] $packageName)

	Write-Host -NoNewline "Installing $packageName..."
	if (brew list $packageName)
	{
		Write-Host " already installed."
	}
	else
	{
		brew install $packageName
		Write-Host " success."
	}
}
