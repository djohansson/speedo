. $PSScriptRoot/../../apt.ps1

Write-Host "Installing Linux dependencies..."

Install-AptPackage autoconf
Install-AptPackage automake 
Install-AptPackage autoconf-archive
Install-AptPackage git
Install-AptPackage pkg-config
Install-AptPackage patchelf
Install-AptPackage lsb-release
Install-AptPackage wget
Install-AptPackage software-properties-common
Install-AptPackage gnupg
#Install-AptPackage llvm
Install-AptPackage libxinerama-dev
Install-AptPackage libxcursor-dev
Install-AptPackage xorg-dev
Install-AptPackage libglu1-mesa-dev

$llvmVersionMajor = 18

wget https://apt.llvm.org/llvm.sh
chmod +x llvm.sh
sudo ./llvm.sh $llvmVersionMajor
rm -f ./llvm.sh

sudo ./update-alternatives-clang.sh $llvmVersionMajor 1

$llvmVersion = $(clang --version | grep "llvm:" | egrep -o '(\d+\.\d+\.\d+-?\w*)')

if (-not ($llvmVersion.Substring(0, $llvmVersion.IndexOf('.')) == $llvmVersionMajor))
{
	Out-Error "Incorrect clang version"
	exit
}

$global:myEnv | Add-Member -Force -PassThru -NotePropertyName VCPKG_FORCE_SYSTEM_BINARIES -NotePropertyValue 1 | Out-Null
$global:myEnv | Add-Member -Force -PassThru -NotePropertyName POWERSHELL_PATH -NotePropertyValue $(which pwsh) | Out-Null
$global:myEnv | Add-Member -Force -PassThru -NotePropertyName LLVM_PATH -NotePropertyValue "/usr" | Out-Null
$global:myEnv | Add-Member -Force -PassThru -NotePropertyName LLVM_VERSION -NotePropertyValue $llvmVersion | Out-Null
$global:myEnv | Add-Member -Force -PassThru -NotePropertyName LLVM_VERSION_MAJOR -NotePropertyValue $llvmVersionMajor | Out-Null
