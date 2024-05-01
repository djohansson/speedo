. $PSScriptRoot/../../homebrew.ps1

Write-Host "Installing MacOS dependencies..."

$(xcode-select --install) 2>&1 | Out-Null

Install-HomebrewPackage pkg-config
Install-HomebrewPackage libxinerama
Install-HomebrewPackage libxcursor
Install-HomebrewPackage mesa-glu
Install-HomebrewPackage patchelf
Install-HomebrewPackage llvm
Install-HomebrewPackage molten-vk

$llvmVersion = $(brew info llvm | grep "llvm:" | egrep -o '(\d+\.\d+\.\d+-?\w*)')

$global:myEnv | Add-Member -Force -PassThru -NotePropertyName MACOS_SDK_PATH -NotePropertyValue $(xcrun --show-sdk-path) | Out-Null
$global:myEnv | Add-Member -Force -PassThru -NotePropertyName POWERSHELL_PATH -NotePropertyValue $($(brew --prefix powershell) + '/bin') | Out-Null
$global:myEnv | Add-Member -Force -PassThru -NotePropertyName LLVM_PATH -NotePropertyValue $(brew --prefix llvm) | Out-Null
$global:myEnv | Add-Member -Force -PassThru -NotePropertyName LLVM_VERSION -NotePropertyValue $llvmVersion | Out-Null
$global:myEnv | Add-Member -Force -PassThru -NotePropertyName LLVM_VERSION_MAJOR -NotePropertyValue $llvmVersion.Substring(0, $llvmVersion.IndexOf('.')) | Out-Null
$global:myEnv | Add-Member -Force -PassThru -NotePropertyName MOLTEN_VK_PATH -NotePropertyValue $(brew --prefix molten-vk) | Out-Null