. $PSScriptRoot/winget.ps1
. $PSScriptRoot/vs.ps1

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
	if (-not ($pwshCmd)) { Write-Error "PowerShell Core installation failed" }
}

$cmakeCmd = Get-Command "cmake" -All -ErrorAction SilentlyContinue | Where-Object Version -GE ([System.Version]"4.2.3.0")
if (-not ($cmakeCmd))
{ 
	Write-Host "Installing CMake..."

	Install-WinGetPackage -Mode Silent -Id Kitware.CMake | Out-Null

	$env:Path = [System.Environment]::GetEnvironmentVariable("Path","Machine") + ";" + [System.Environment]::GetEnvironmentVariable("Path","User") 

	$cmakeCmd = Get-Command "cmake" -All -ErrorAction SilentlyContinue | Where-Object Version -GE ([System.Version]"4.2.3.0")
	if (-not ($cmakeCmd)) { Write-Error "CMake 4.2.3+ installation failed" }
}

$VSSetupInstance = Get-VSSetupInstance | Select-VSSetupInstance -Product * -Require "Microsoft.VisualStudio.Workload.VCTools","Microsoft.VisualStudio.Component.VC.ATL","Microsoft.VisualStudio.Component.VC.Llvm.Clang","Microsoft.VisualStudio.Component.Windows11SDK.22621"
if (-not ($VSSetupInstance))
{
	Write-Host "Installing VisualStudio 2022 VC BuildTools..."

	# --force is required to circumvent the fact that Microsoft.VisualStudio.2022.BuildTools could already be installed without the VCTools workload
	winget install -e -h --id Microsoft.VisualStudio.2022.BuildTools --override "--quiet --add Microsoft.VisualStudio.Workload.VCTools;includeRecommended --add Microsoft.VisualStudio.Component.VC.ATL --add Microsoft.VisualStudio.Component.VC.Llvm.Clang --add Microsoft.VisualStudio.Component.Windows11SDK.22621 --wait" --force

	$VSSetupInstance = Get-VSSetupInstance | Select-VSSetupInstance -Product * -Require "Microsoft.VisualStudio.Workload.VCTools","Microsoft.VisualStudio.Component.VC.ATL","Microsoft.VisualStudio.Component.VC.Llvm.Clang","Microsoft.VisualStudio.Component.Windows11SDK.22621"
	if (-not ($VSSetupInstance)) { Write-Error "Visual Studio 2022 VC BuildTools installation failed (winget exit code $LASTEXITCODE)" }
}

$winSDKManifest = [xml](Get-Content -Path "C:\Program Files (x86)\Windows Kits\10\SDKManifest.xml")
$platformIndentityStr = $winSDKManifest.FileList.PlatformIdentity
$windowsSdkVersion = $platformIndentityStr.SubString($platformIndentityStr.LastIndexOf("Version=") + 8)

$global:myEnv['WINDOWS_SDK'] = "C:\Program Files (x86)\Windows Kits\10"
$global:myEnv['WINDOWS_SDK_VERSION'] = $windowsSdkVersion
$global:myEnv['VISUAL_STUDIO_PATH'] = $VSSetupInstance.InstallationPath
$global:myEnv['VISUAL_STUDIO_VCTOOLS_VERSION'] = (Get-Content -Path ($VSSetupInstance.InstallationPath + "\VC\Auxiliary\Build\Microsoft.VCToolsVersion.default.txt"))
