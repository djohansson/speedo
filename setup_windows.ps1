Write-Host "Installing Windows dependencies..."

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
	
$gitCmd = Get-Command "git" -All -ErrorAction SilentlyContinue | Where-Object Version -GE ([System.Version]"2.41.0.0")
if (-not ($gitCmd))
{
	Write-Host "Installing Git..."

	Install-WinGetPackage -Mode Silent -Id Git.Git | Out-Null

	$gitCmd = Get-Command "git" -All -ErrorAction SilentlyContinue | Where-Object Version -GE ([System.Version]"2.41.0.0")
}

$vulkanSdkInfo = Get-WinGetPackage KhronosGroup.VulkanSDK
if (-not ($vulkanSdkInfo) -or ($vulkanSdkInfo.InstalledVersion -lt ([System.Version]"1.3.268.0")))
{
	Write-Host "Installing VulkanSDK (requires process elevation)..."

	Start-Process pwsh -Verb runas -ArgumentList "-c Install-WinGetPackage -Mode Silent -Id KhronosGroup.VulkanSDK | Out-Null" -Wait

	$vulkanSdkInfo = Get-WinGetPackage KhronosGroup.VulkanSDK
}

$windowsSdkInfo = Get-WinGetPackage Microsoft.WindowsSDK.10.0.22621
if (-not ($windowsSdkInfo))
{
	Write-Host "Installing WindowsSDK 10.0.22000 (requires process elevation)..."

	Start-Process pwsh -Verb runas -ArgumentList "-c Install-WinGetPackage -Mode Silent -Id Microsoft.WindowsSDK.10.0.22621 | Out-Null" -Wait

	$windowsSdkInfo = Get-WinGetPackage Microsoft.WindowsSDK.10.0.22621
}
$winSDKManifest = [xml](Get-Content -Path "C:\Program Files (x86)\Windows Kits\10\SDKManifest.xml")
$platformIndentityStr = $winSDKManifest.FileList.PlatformIdentity
$windowsSdkVersion = $platformIndentityStr.SubString($platformIndentityStr.LastIndexOf("Version=") + 8)

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

$llvmInfo = Get-WinGetPackage LLVM.LLVM
if (-not ($llvmInfo) -or ($llvmInfo.InstalledVersion -lt ([System.Version]"17.0.0")))
{
	Write-Host "Installing LLVM (requires process elevation)..."

	Install-WinGetPackage -Mode Silent -Id LLVM.LLVM | Out-Null

	$llvmInfo = Get-WinGetPackage LLVM.LLVM
}
$llvmVersion = $llvmInfo.InstalledVersion
$llvmVersionShort = $llvmVersion.Substring(0, $llvmVersion.IndexOf('.')) #workaround for paths not matching the full version number

$global:myEnv | Add-Member -Force -PassThru -NotePropertyName POWERSHELL_PATH -NotePropertyValue (Split-Path -Path $pwshCmd.Source) | Out-Null
$global:myEnv | Add-Member -Force -PassThru -NotePropertyName GIT_PATH -NotePropertyValue (Split-Path -Path $gitCmd.Source) | Out-Null
$global:myEnv | Add-Member -Force -PassThru -NotePropertyName VULKAN_SDK -NotePropertyValue ("C:\VulkanSDK\" + $vulkanSdkInfo.InstalledVersion) | Out-Null
$global:myEnv | Add-Member -Force -PassThru -NotePropertyName VULKAN_SDK_VERSION -NotePropertyValue $vulkanSdkInfo.InstalledVersion | Out-Null
$global:myEnv | Add-Member -Force -PassThru -NotePropertyName WINDOWS_SDK -NotePropertyValue "C:\Program Files (x86)\Windows Kits\10" | Out-Null
$global:myEnv | Add-Member -Force -PassThru -NotePropertyName WINDOWS_SDK_VERSION -NotePropertyValue $windowsSdkVersion | Out-Null
$global:myEnv | Add-Member -Force -PassThru -NotePropertyName VISUAL_STUDIO_PATH -NotePropertyValue $VSSetupInstance.InstallationPath | Out-Null
$global:myEnv | Add-Member -Force -PassThru -NotePropertyName VISUAL_STUDIO_VCTOOLS_VERSION -NotePropertyValue (Get-Content -Path ($VSSetupInstance.InstallationPath + "\VC\Auxiliary\Build\Microsoft.VCToolsVersion.default.txt")) | Out-Null
$global:myEnv | Add-Member -Force -PassThru -NotePropertyName LLVM_PATH -NotePropertyValue "C:\Program Files\LLVM" | Out-Null
$global:myEnv | Add-Member -Force -PassThru -NotePropertyName LLVM_VERSION -NotePropertyValue $llvmVersionShort | Out-Null
