function Install-AptPackage
{
	param([Parameter(Mandatory = $True, Position = 0)] [string] $packageName)

	Write-Host "Installing $packageName..."
	if (dpkg -s $packageName)
	{
		Write-Host "$packageName is already installed."
	}
	else
	{
		sudo apt -y install $packageName
		if ($LASTEXITCODE -ne 0) { throw "apt install $packageName failed with exit code $LASTEXITCODE" }
		Write-Host "$packageName is installed."
	}
}
