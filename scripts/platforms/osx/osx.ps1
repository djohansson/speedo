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
Install-HomebrewPackage ninja
Install-HomebrewPackage cmake

$global:myEnv | Add-Member -Force -PassThru -NotePropertyName MACOS_SDK_PATH -NotePropertyValue $(xcrun --show-sdk-path) | Out-Null
$global:myEnv | Add-Member -Force -PassThru -NotePropertyName POWERSHELL_PATH -NotePropertyValue $($(brew --prefix powershell) + '/bin') | Out-Null
$global:myEnv | Add-Member -Force -PassThru -NotePropertyName CMAKE_APPLE_SILICON_PROCESSOR -NotePropertyValue $(Get-NativeArchitecture) | Out-Null