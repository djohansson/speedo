. $PSScriptRoot/../../winget.ps1
. $PSScriptRoot/../../vs.ps1

Write-Host "Installing Windows dependencies..."

Install-WinGetClient
Install-VSSetup

$pwshCmd = Get-Command "pwsh" -All -ErrorAction SilentlyContinue | Where-Object Version -GE ([System.Version]"7.5.0.0")
if (-not ($pwshCmd))
{ 
	Write-Host "Installing Powershell Core..."

	Install-WinGetPackage -Mode Silent -Id Microsoft.PowerShell | Out-Null

	$env:Path = [System.Environment]::GetEnvironmentVariable("Path","Machine") + ";" + [System.Environment]::GetEnvironmentVariable("Path","User") 

	$pwshCmd = Get-Command "pwsh" -All -ErrorAction SilentlyContinue | Where-Object Version -GE ([System.Version]"7.0.0.0")
}
	
$gitCmd = Get-Command "git" -All -ErrorAction SilentlyContinue | Where-Object Version -GE ([System.Version]"2.49.0.0")
if (-not ($gitCmd))
{
	Write-Host "Installing Git..."

	Install-WinGetPackage -Mode Silent -Id Git.Git | Out-Null

	$env:Path = [System.Environment]::GetEnvironmentVariable("Path","Machine") + ";" + [System.Environment]::GetEnvironmentVariable("Path","User") 

	$gitCmd = Get-Command "git" -All -ErrorAction SilentlyContinue | Where-Object Version -GE ([System.Version]"2.49.0.0")
}

$ninjaCmd = Get-Command "ninja" -All -ErrorAction SilentlyContinue
if (-not ($ninjaCmd))
{ 
	Write-Host "Installing Ninja..."

	Install-WinGetPackage -Mode Silent -Id Ninja-build.Ninja | Out-Null

	$env:Path = [System.Environment]::GetEnvironmentVariable("Path","Machine") + ";" + [System.Environment]::GetEnvironmentVariable("Path","User") 

	$ninjaCmd = Get-Command "ninja" -All -ErrorAction SilentlyContinue
}

$cmakeCmd = Get-Command "cmake" -All -ErrorAction SilentlyContinue | Where-Object Version -GE ([System.Version]"3.31.6.0")
if (-not ($cmakeCmd))
{ 
	Write-Host "Installing Cmake..."

	Install-WinGetPackage -Mode Silent -Id Kitware.CMake | Out-Null

	$env:Path = [System.Environment]::GetEnvironmentVariable("Path","Machine") + ";" + [System.Environment]::GetEnvironmentVariable("Path","User") 

	$cmakeCmd = Get-Command "cmake" -All -ErrorAction SilentlyContinue | Where-Object Version -GE ([System.Version]"3.31.6.0")
}

$windowsSdkInfo = Get-WinGetPackage Microsoft.WindowsSDK.10.0.26100
if (-not ($windowsSdkInfo))
{
	Write-Host "Installing WindowsSDK 10.0.26100 (requires process elevation)..."

	Start-Process pwsh -Verb runas -ArgumentList "-c Install-WinGetPackage -Mode Silent -Id Microsoft.WindowsSDK.10.0.26100 | Out-Null" -Wait

	$windowsSdkInfo = Get-WinGetPackage Microsoft.WindowsSDK.10.0.26100
}
$winSDKManifest = [xml](Get-Content -Path "C:\Program Files (x86)\Windows Kits\10\SDKManifest.xml")
$platformIndentityStr = $winSDKManifest.FileList.PlatformIdentity
$windowsSdkVersion = $platformIndentityStr.SubString($platformIndentityStr.LastIndexOf("Version=") + 8)

$VSSetupInstance = Get-VSSetupInstance | Select-VSSetupInstance -Product * -Require "Microsoft.VisualStudio.Workload.VCTools","Microsoft.VisualStudio.Component.VC.ATL","Microsoft.VisualStudio.Component.VC.Llvm.Clang"
if (-not ($VSSetupInstance))
{
	Write-Host "Installing VisualStudio 2022 VC BuildTools..."

	# --force is required to circumvent the fact that Microsoft.VisualStudio.2022.BuildTools could already be installed without the VCTools workload
	winget install -e -h --id Microsoft.VisualStudio.2022.BuildTools --override "--quiet --add Microsoft.VisualStudio.Workload.VCTools;includeRecommended --add Microsoft.VisualStudio.Component.VC.ATL --add Microsoft.VisualStudio.Component.VC.Llvm.Clang --wait" --force

	$VSSetupInstance = Get-VSSetupInstance | Select-VSSetupInstance -Product * -Require "Microsoft.VisualStudio.Workload.VCTools"
}

$global:myEnv | Add-Member -Force -PassThru -NotePropertyName POWERSHELL_PATH -NotePropertyValue (Split-Path -Path $pwshCmd.Source) | Out-Null
$global:myEnv | Add-Member -Force -PassThru -NotePropertyName GIT_PATH -NotePropertyValue (Split-Path -Path $gitCmd.Source) | Out-Null
$global:myEnv | Add-Member -Force -PassThru -NotePropertyName WINDOWS_SDK -NotePropertyValue "C:\Program Files (x86)\Windows Kits\10" | Out-Null
$global:myEnv | Add-Member -Force -PassThru -NotePropertyName WINDOWS_SDK_VERSION -NotePropertyValue $windowsSdkVersion | Out-Null
$global:myEnv | Add-Member -Force -PassThru -NotePropertyName VISUAL_STUDIO_PATH -NotePropertyValue $VSSetupInstance.InstallationPath | Out-Null
$global:myEnv | Add-Member -Force -PassThru -NotePropertyName VISUAL_STUDIO_VCTOOLS_VERSION -NotePropertyValue (Get-Content -Path ($VSSetupInstance.InstallationPath + "\VC\Auxiliary\Build\Microsoft.VCToolsVersion.default.txt")) | Out-Null
