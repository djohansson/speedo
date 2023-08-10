function LoadEnv
{
	param(
		[Parameter(Mandatory = $True, Position = 0)] [string] $firstArg,
		[Parameter(Mandatory = $True, ValueFromRemainingArguments = $true, Position = 1)][string[]] $listArgs
	)

	$myEnvFile = "$PSScriptRoot/env.json"
	$myEnv = Get-Content -Path $myEnvFile -Raw | ConvertFrom-Json

	foreach( $property in $myEnv.psobject.properties)
	{
		[Environment]::SetEnvironmentVariable($property.Name, $property.Value)
	}

	$secondArg = Join-String -Separator " " -InputObject $listArgs

	Invoke-Expression ($firstArg + " " + $secondArg)
}