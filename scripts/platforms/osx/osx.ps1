. $PSScriptRoot/../../homebrew.ps1
. $PSScriptRoot/../../platform.ps1

Write-Host "Installing MacOS dependencies..."

$(xcode-select --install) 2>&1

Install-HomebrewPackage coreutils
Install-HomebrewPackage pkg-config
Install-HomebrewPackage libxinerama
Install-HomebrewPackage libxxf86vm
Install-HomebrewPackage libxcursor
Install-HomebrewPackage mesa-glu
Install-HomebrewPackage patchelf
Install-HomebrewPackage molten-vk

$global:myEnv | Add-Member -Force -PassThru -NotePropertyName MACOS_SDK_PATH -NotePropertyValue $(xcrun --show-sdk-path) | Out-Null
$global:myEnv | Add-Member -Force -PassThru -NotePropertyName APPLE_SILICON_PROCESSOR -NotePropertyValue $(Get-HostArchitecture) | Out-Null