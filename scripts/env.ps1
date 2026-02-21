. $PSScriptRoot/platform.ps1

function Set-EnvVariable
{
	param(
		[Parameter(Mandatory = $True)] [string] $Key,
		[Parameter(Mandatory = $True)] [string] $Value,
		[Parameter(Mandatory = $False)] [EnvironmentVariableTarget] $Scope = [EnvironmentVariableTarget]::Process
	)

	[Environment]::SetEnvironmentVariable($Key, $Value, $Scope)
}

function Add-EnvPath
{
	param(
		[Parameter(Mandatory = $True)] [string] $Path,
		[Parameter(Mandatory = $False)] [EnvironmentVariableTarget] $Scope = [EnvironmentVariableTarget]::Process
	)

	if (-not (Test-Path $Path))
	{
		Write-Warning "Path does not exist: $Path"
	}

	if ("" -eq $env:PATH -or $null -eq $env:PATH)
	{
		Set-EnvVariable -Key "PATH" -Value $Path -Scope $Scope
	}
	else
	{
		Set-EnvVariable -Key "PATH" -Value $($env:PATH + [IO.Path]::PathSeparator + $Path) -Scope $Scope
	}
}

function Add-EnvDylibPath
{
	param(
		[Parameter(Mandatory = $True)] [string] $Path,
		[Parameter(Mandatory = $False)] [EnvironmentVariableTarget] $Scope = [EnvironmentVariableTarget]::Process
	)

	if (-not (Test-Path $Path))
	{
		Write-Warning "Path does not exist: $Path"
	}

	if (!(Test-Path Variable:\IsWindows) -or $IsWindows) 
	{
		Add-EnvPath $Path $Scope
	}
	elseif ($IsMacOS)
	{
		if ("" -eq $env:DYLD_LIBRARY_PATH -or $null -eq $env:DYLD_LIBRARY_PATH)
		{
			[Environment]::SetEnvironmentVariable("DYLD_LIBRARY_PATH", $Path, $Scope)
		}
		else
		{
			[Environment]::SetEnvironmentVariable("DYLD_LIBRARY_PATH", $env:DYLD_LIBRARY_PATH + [IO.Path]::PathSeparator + $Path, $Scope)
		}
	}
	elseif ($IsLinux)
	{
		if ("" -eq $env:LD_LIBRARY_PATH -or $null -eq $env:LD_LIBRARY_PATH)
		{
			[Environment]::SetEnvironmentVariable("LD_LIBRARY_PATH", $Path, $Scope)
		}
		else
		{
			[Environment]::SetEnvironmentVariable("LD_LIBRARY_PATH", $env:LD_LIBRARY_PATH + [IO.Path]::PathSeparator + $Path, $Scope)
		}
	}
	else
	{
		Write-Error "Unsupported Operating System" # please implement me
	}
}

function Read-EnvFile
{
	param(
		[Parameter(Mandatory = $True)] [string] $EnvFile,
		[Parameter(Mandatory = $False)] [EnvironmentVariableTarget] $Scope = [EnvironmentVariableTarget]::Process
	)

	if (-not (Test-Path $EnvFile))
	{
		Write-Error "$EnvFile does not exist"

		return
	}
	
	$localEnv = Get-Content -Path $EnvFile -Raw | ConvertFrom-Json

	foreach($property in $localEnv.psobject.properties)
	{
		Set-EnvVariable $property.Name $property.Value $Scope

		#Write-Host $property.Name "=" $property.Value
	}
}

function Invoke-Sudo
{ 
	& /usr/bin/env sudo pwsh -command "& $args" 
}

function Initialize-VcpkgEnv
{
	param(
		[Parameter(Mandatory = $False)] [string] $TargetTriplet
	)
	if (-not $TargetTriplet)
	{
		$TargetTriplet = Get-HostTriplet
	}

	$packageRoot = "$PSScriptRoot/../build/packages/$TargetTriplet"
	
	if (!(Test-Path Variable:\IsWindows) -or $IsWindows) 
	{
		Add-EnvDylibPath "$packageRoot/bin"
		Add-EnvDylibPath "$packageRoot/debug/bin"
	}
	else
	{
		Add-EnvDylibPath "$packageRoot/lib"
		Add-EnvDylibPath "$packageRoot/debug/lib"
	}
}

function Initialize-HostUserEnv
{
	#Write-Host "Setting system environment variables from env.json"
	Read-EnvFile "$PSScriptRoot/../.env.json" User
}

function Initialize-ToolchainEnv
{
	param(
		[Parameter(Mandatory = $False)] [string] $TargetTriplet
	)
	if (-not $TargetTriplet)
	{
		$TargetTriplet = Get-HostTriplet
	}

	$toolchainRoot = "$PSScriptRoot/../build/toolchain/$TargetTriplet"
	$Env:LLVM_ROOT = $toolchainRoot

	#Write-Host "Adding toolchain dylib/dll/so:s..."
	if (!(Test-Path Variable:\IsWindows) -or $IsWindows) 
	{
		$dynlibPath = "$toolchainRoot/bin"
	}
	else
	{
		$dynlibPath = "$toolchainRoot/lib"
	}
	$toolsPath = "$toolchainRoot/tools/llvm"
	$Env:LLVM_TOOLS_PATH = $toolchainRoot
	
	#Write-Host "Adding $dynlibPath"
	if (Test-Path $dynlibPath)
	{
		#Write-Host "Adding $dynlibPath"
		Add-EnvDylibPath $dynlibPath
	}
	#Write-Host "Adding $toolsPath"
	Add-EnvPath $toolsPath
}

function Initialize-VcpkgToolsEnv
{ 
	param(
		[Parameter(Mandatory = $False)] [string] $TargetTriplet
	)
	if (-not $TargetTriplet)
	{
		$TargetTriplet = Get-TargetTriplet
		Write-Host "Target triplet not specified, using default: $TargetTriplet"
	}
	else
	{
		Write-Host "Using target triplet: $TargetTriplet"
	}

	foreach($toolsPath in Get-ChildItem -Path $PSScriptRoot/../build/packages/$TargetTriplet/tools -Directory -ErrorAction SilentlyContinue)
	{
		Add-EnvPath $toolsPath

		if ($IsLinux -or $IsMacOS)
		{
			foreach ($tool in Get-ChildItem $toolsPath -File -Recurse -ErrorAction SilentlyContinue)
			{
				#Write-Host "Granting executable rights to " $tool 
				Invoke-Expression "chmod +x $tool"
				#Invoke-Sudo "chmod +x $tool"
			}
		}
	}
}

function Invoke-VcpkgEnv
{
	param(
		[Parameter(Mandatory = $True, Position = 0)] [string] $Command,
		[Parameter(Mandatory = $True, Position = 1)] [string] $TargetTriplet,
		[Parameter(Mandatory = $True, ValueFromRemainingArguments = $true, Position = 1)][string[]] $ArgsList
	)

	Initialize-VcpkgEnv $TargetTriplet

	$arguments = Join-String -Separator " " -InputObject $ArgsList
	$expr = ($command + " " + $arguments)

	#Write-Host $expr
	Invoke-Expression $expr
}
