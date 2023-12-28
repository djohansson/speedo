. $PSScriptRoot\homebrew.ps1

Write-Host "Installing MacOS dependencies..."

$(xcode-select --install) 2>&1 | Out-Null
$global:myEnv | Add-Member -Force -PassThru -NotePropertyName MACOS_SDK_PATH -NotePropertyValue $(xcrun --show-sdk-path) | Out-Null

$pwshPath = $(brew --prefix powershell)
$global:myEnv | Add-Member -Force -PassThru -NotePropertyName POWERSHELL_PATH -NotePropertyValue $pwshPath/bin | Out-Null

$vulkanPath = Get-Command "vulkaninfo" -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Source | Split-Path | Split-Path
if (-not ($vulkanPath))
{
	# TODO: install vulkan sdk
	#curl -O https://sdk.lunarg.com/sdk/download/latest/mac/vulkan_sdk.dmg
	#hdiutil attach vulkan_sdk.dmg
	#sudo ./InstallVulkan.app/Contents/MacOS/InstallVulkan --root $(mkdir build.vulkan; cd ./build.vulkan ; pwd) --accept-licenses --default-answer --confirm-command install
	#hdiutil detach /Volumes/vulkansdk-macos-*
	#rm vulkan_sdk.dmg
	#sudo python3 ~/VulkanSDK/*/install_vulkan.py --install-json-location ~/VulkanSDK/*/
}
$global:myEnv | Add-Member -Force -PassThru -NotePropertyName VULKAN_SDK -NotePropertyValue $vulkanPath | Out-Null

Install-HomebrewPackage pkg-config
Install-HomebrewPackage libxinerama
Install-HomebrewPackage libxcursor
Install-HomebrewPackage mesa-glu
Install-HomebrewPackage python
Install-HomebrewPackage patchelf

Install-HomebrewPackage llvm
$llvmPath = $(brew --prefix llvm)
$global:myEnv | Add-Member -Force -PassThru -NotePropertyName LLVM_PATH -NotePropertyValue $llvmPath | Out-Null
$global:myEnv | Add-Member -Force -PassThru -NotePropertyName LLVM_VERSION -NotePropertyValue $(brew info llvm | grep "llvm:" | egrep -o '(\d+\.\d+\.\d+-?\w*)') | Out-Null