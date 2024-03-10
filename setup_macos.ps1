. $PSScriptRoot\homebrew.ps1

Write-Host "Installing MacOS dependencies..."

$(xcode-select --install) 2>&1 | Out-Null
$global:myEnv | Add-Member -Force -PassThru -NotePropertyName MACOS_SDK_PATH -NotePropertyValue $(xcrun --show-sdk-path) | Out-Null

$pwshPath = $(brew --prefix powershell)
$global:myEnv | Add-Member -Force -PassThru -NotePropertyName POWERSHELL_PATH -NotePropertyValue $pwshPath/bin | Out-Null

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