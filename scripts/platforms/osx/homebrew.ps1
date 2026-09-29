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
		if ($LASTEXITCODE -ne 0) { throw "brew install $packageName failed with exit code $LASTEXITCODE" }
		Write-Host " success."
	}
}
