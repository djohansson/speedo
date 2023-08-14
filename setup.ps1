. $PSScriptRoot/env.ps1

$myEnvFile = "$PSScriptRoot/env.json"
$myEnv = New-Object -TypeName PSObject
$myTriplet = Get-Triplet

if (Test-Path $myEnvFile)
{
	$myEnv = Get-Content -Path $myEnvFile -Raw | ConvertFrom-Json
}

Write-Host "Installing dependencies..."

if ($IsWindows)
{
	if (-not (Get-InstalledModule Microsoft.WinGet.Client -ErrorAction SilentlyContinue))
	{
		Write-Host "Installing Microsoft.WinGet.Client package..."
	
		Install-Package Microsoft.WinGet.Client -Scope CurrentUser -Force -Confirm:$False | Out-Null
	}

	$pwshCmd = Get-Command "pwsh" -All -ErrorAction SilentlyContinue | Where-Object Version -GE ([System.Version]"7.0.0.0")

	if (-not ($pwshCmd))
	{ 
		Write-Host "Installing Powershell Core..."

		Install-WinGetPackage -Mode Silent -Id Microsoft.PowerShell | Out-Null

		$pwshCmd = Get-Command "pwsh" -All -ErrorAction SilentlyContinue | Where-Object Version -GE ([System.Version]"7.0.0.0")
	}
	$myEnv | Add-Member -Force -PassThru -NotePropertyName POWERSHELL_PATH -NotePropertyValue (Split-Path -Path $pwshCmd.Source) | Out-Null
		
	$gitCmd = Get-Command "git" -All -ErrorAction SilentlyContinue | Where-Object Version -GE ([System.Version]"2.41.0.0")
	
	if (-not ($gitCmd))
	{
		Write-Host "Installing Git..."

		Install-WinGetPackage -Mode Silent -Id Git.Git | Out-Null

		$gitCmd = Get-Command "git" -All -ErrorAction SilentlyContinue | Where-Object Version -GE ([System.Version]"2.41.0.0")
	}
	$myEnv | Add-Member -Force -PassThru -NotePropertyName GIT_PATH -NotePropertyValue (Split-Path -Path $gitCmd.Source) | Out-Null

	$vulkanSdkInfo = Get-WinGetPackage KhronosGroup.VulkanSDK
	$vulkanSdkInfoHashTable = @{}
	$vulkanSdkInfo.psobject.properties | ForEach-Object { $vulkanSdkInfoHashTable[$_.Name] = $_.Value }

	if (-not ($vulkanSdkInfo) -or ($vulkanSdkInfo.InstalledVersion -lt ([System.Version]"1.3.231.1")))
	{
		Write-Host "Installing VulkanSDK (requires process elevation)..."

		Start-Process pwsh -Verb runas -ArgumentList "-c Install-WinGetPackage -Mode Silent -Id KhronosGroup.VulkanSDK | Out-Null"
	}
	$myEnv | Add-Member -Force -PassThru -NotePropertyName VULKAN_SDK -NotePropertyValue "C:\VulkanSDK\1.3.231.1" | Out-Null
	$myEnv | Add-Member -Force -PassThru -NotePropertyName VULKAN_SDK_VERSION -NotePropertyValue $vulkanSdkInfoHashTable["InstalledVersion"] | Out-Null

	$windowsSdkInfo = Get-WinGetPackage Microsoft.WindowsSDK.10.0.22621
	$windowsSdkInfoHashTable = @{}
	$windowsSdkInfo.psobject.properties | ForEach-Object { $windowsSdkInfoHashTable[$_.Name] = $_.Value }
	$windowsSdkVersion = $windowsSdkInfoHashTable["InstalledVersion"]
	$windowsSdkVersionShort = $windowsSdkVersion.SubString(0, $windowsSdkVersion.LastIndexOf('.')) + ".0" #workaround for paths not matching the full version number

	if (-not ($windowsSdkInfo))
	{
		Write-Host "Installing WindowsSDK 10.0.22621 (requires process elevation)..."

		Start-Process pwsh -Verb runas -ArgumentList "-c Install-WinGetPackage -Mode Silent -Id Microsoft.WindowsSDK.10.0.22621 | Out-Null"
	}
	$myEnv | Add-Member -Force -PassThru -NotePropertyName WINDOWS_SDK -NotePropertyValue "C:\Program Files (x86)\Windows Kits\10" | Out-Null
	$myEnv | Add-Member -Force -PassThru -NotePropertyName WINDOWS_SDK_VERSION -NotePropertyValue $windowsSdkVersionShort | Out-Null

	if (-not (Get-InstalledModule VSSetup -ErrorAction SilentlyContinue))
	{
		Write-Host "Installing VSSetup module..."
	
		Install-Module VSSetup -Scope CurrentUser -Confirm:$False -Force
	}

	$VSSetupInstance = Get-VSSetupInstance | Select-VSSetupInstance -Product * -Require "Microsoft.VisualStudio.Workload.VCTools"

	if (-not ($VSSetupInstance))
	{
		Write-Host "Installing VisualStudio 2022 VC BuildTools..."

		# --force is required to circumvent the fact that Microsoft.VisualStudio.2022.BuildTools could already be installed without the VCTools workload
		winget install -e -h --id Microsoft.VisualStudio.2022.BuildTools --override "--quiet --add Microsoft.VisualStudio.Workload.VCTools;includeRecommended --wait" --force

		$VSSetupInstance = Get-VSSetupInstance | Select-VSSetupInstance -Product * -Require "Microsoft.VisualStudio.Workload.VCTools"
	}
	$myEnv | Add-Member -Force -PassThru -NotePropertyName VISUAL_STUDIO_PATH -NotePropertyValue $VSSetupInstance.InstallationPath | Out-Null
	$myEnv | Add-Member -Force -PassThru -NotePropertyName VISUAL_STUDIO_VCTOOLS_VERSION -NotePropertyValue (Get-Content -Path ($VSSetupInstance.InstallationPath + "\VC\Auxiliary\Build\Microsoft.VCToolsVersion.default.txt")) | Out-Null

	$llvmInfo = Get-WinGetPackage LLVM.LLVM
	$llvmInfoHashTable = @{}
	$llvmInfo.psobject.properties | ForEach-Object { $llvmInfoHashTable[$_.Name] = $_.Value }
	$llvmVersion = $llvmPkgHashTable["InstalledVersion"]
	$llvmVersionShort = $llvmVersion.Substring(0, $llvmVersion.IndexOf('.')) #workaround for paths not matching the full version number

	if (-not ($llvmInfo) -or ($llvmInfo.InstalledVersion -lt ([System.Version]"16.0.6"))) # todo: set this to 17.0.0 once it's available
	{
		Write-Host "Installing LLVM (requires process elevation)..."

		Install-WinGetPackage -Mode Silent -Id LLVM.LLVM | Out-Null
	}
	$myEnv | Add-Member -Force -PassThru -NotePropertyName LLVM_PATH -NotePropertyValue "C:\Program Files\LLVM" | Out-Null
	$myEnv | Add-Member -Force -PassThru -NotePropertyName LLVM_VERSION -NotePropertyValue $llvmVersionShort | Out-Null
}

$myEnv | ConvertTo-Json | Out-File $myEnvFile

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

Write-Host "Installing vcpkg packages for $myTriplet using manifest..."

vcpkg install --x-install-root="build.vcpkg-installed" --overlay-triplets=$PSScriptRoot --triplet $myTriplet --x-feature=client --x-feature=server --x-feature=tests --no-print-usage

Initialize-DevEnv
