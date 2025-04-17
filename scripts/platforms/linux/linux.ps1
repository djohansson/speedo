. $PSScriptRoot/../../apt.ps1

Write-Host "Installing Linux dependencies..."

Install-AptPackage autoconf
Install-AptPackage libtool
Install-AptPackage coreutils
Install-AptPackage pkg-config
Install-AptPackage git
Install-AptPackage cmake
Install-AptPackage ninja-build
Install-AptPackage patchelf
Install-AptPackage lsb-release
Install-AptPackage software-properties-common
Install-AptPackage gnupg
Install-AptPackage libxinerama-dev
Install-AptPackage libxcursor-dev
Install-AptPackage libglu1-mesa-dev
Install-AptPackage libwayland-dev
Install-AptPackage xorg-dev
Install-AptPackage libxkbcommon-dev

$global:myEnv | Add-Member -Force -PassThru -NotePropertyName POWERSHELL_PATH -NotePropertyValue $(which pwsh) | Out-Null

$env:VCPKG_FORCE_SYSTEM_BINARIES = "1"

