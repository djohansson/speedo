$Arch = $Env:PROCESSOR_ARCHITECTURE
$Triplet = ""

if ($Arch -eq "AMD64")
{
	$Triplet += "x64-"
}
# elseif ($Arch -eq "x86")
# {
# 	$Triplet += "x86-"
# }
# elseif ($Arch -eq "ARM64")
# {
# 	$Triplet += "arm64-"
# }
else
{
	Write-Error "Unsupported architecture: $Arch"
	
	return
}

if ($IsWindows)
{
	$Triplet += "windows"
}
# elseif ($IsMacOS)
# {
# 	$Triplet += "osx"
# }
# elseif ($IsLinux)
# {
# 	$Triplet += "linux"
# }
else
{
	Write-Error "Unsupported Operating System"
	
	return
}

$Triplet += "-clang"

if ($IsWindows)
{
	$gitListResult = winget list --id Git.Git -e
	
	if ($LASTEXITCODE)
	{
		Write-Host "Installing Git..."

		winget install -e -h --id Git.Git
	}
	else
	{
		$gitVersion = (-split $gitListResult[-1])[-2]

		if (-not $(git --version) -like $gitVersion)
		{
			Write-Warning "Git avaliable from PATH does not match the installed version by this script."
		}
	}

	winget list --id LLVM.LLVM -e | Out-Null
	
	if ($LASTEXITCODE)
	{
		Write-Host "Installing LLVM..."

		winget install -e -h --id LLVM.LLVM
	}

	if (-not (Get-InstalledModule VSSetup -ErrorAction 'SilentlyContinue'))
	{
		Write-Host "Installing VSSetup powershell module..."
	
		Install-Module VSSetup -Scope CurrentUser -Confirm:$False -Force
	}

	$VSSetupInstance = Get-VSSetupInstance | Select-VSSetupInstance -Product * -Require "Microsoft.VisualStudio.Workload.VCTools"

	if (-not ($VSSetupInstance))
	{
		Write-Host "Installing VisualStudio 2022 VC BuildTools..."

		winget install -e -h --id Microsoft.VisualStudio.2022.BuildTools --override "--quiet --add Microsoft.VisualStudio.Workload.VCTools;includeRecommended --wait" --force
	}
}

Write-Host "Setting up vcpkg environment..."

if (Test-Path -Path "~/.vcpkg")
{
	. ~/.vcpkg/vcpkg-init.ps1
}
else
{
	Write-Host "Installing vcpkg..."

	Invoke-Expression(Invoke-WebRequest -useb https://aka.ms/vcpkg-init.ps1)
}

Write-Host "Installing vcpkg packages for $Triplet using manifest..."

vcpkg install --overlay-triplets=$PSScriptRoot --triplet $Triplet --x-feature=client --x-feature=server --x-feature=tests --no-print-usage

Write-Host "Adding installed vcpkg packages to path..."

$BinDirectory = "$PSScriptRoot\vcpkg_installed\$Triplet\bin"
$env:Path += ";$BinDirectory"

Write-Host $BinDirectory

foreach($ToolsDirectory in Get-ChildItem -Path $PSScriptRoot\vcpkg_installed\$Triplet\tools -Directory)
{
	$env:Path += ";$ToolsDirectory"

	Write-Host $ToolsDirectory
}
