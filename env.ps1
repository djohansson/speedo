function Read-EnvFile
{
	param([Parameter(Mandatory = $True, Position = 0)] [string] $envFile)

	$localEnv = Get-Content -Path $envFile -Raw | ConvertFrom-Json

	foreach( $property in $localEnv.psobject.properties)
	{
		Write-Host $property.Name "=" $property.Value
		[Environment]::SetEnvironmentVariable($property.Name, $property.Value)
	}
}

function Initialize-DevEnv
{
	Write-Host "Adding installed vcpkg packages to env:Path..."

	$BinDirectory = "$PSScriptRoot\build.vcpkg-installed\$Triplet\bin"
	$env:Path += ";$BinDirectory"

	Write-Host $BinDirectory

	foreach($ToolsDirectory in Get-ChildItem -Path $PSScriptRoot\build.vcpkg-installed\$Triplet\tools -Directory)
	{
		$env:Path += ";$ToolsDirectory"

		Write-Host $ToolsDirectory
	}
	
	Write-Host "Setting environment variables..."

	Read-EnvFile "$PSScriptRoot/env.json"
}
