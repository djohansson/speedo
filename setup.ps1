. $PSScriptRoot/scripts/env.ps1
. $PSScriptRoot/scripts/platform.ps1

$global:myEnv = New-Object -TypeName PSObject

$myEnvFile = "$PSScriptRoot/.env.json"

if (Test-Path $myEnvFile)
{
	$global:myEnv = Get-Content -Path $myEnvFile -Raw | ConvertFrom-Json -AsHashtable
}

if (!(Test-Path Variable:\IsWindows) -or $IsWindows)
{
	& $PSScriptRoot/scripts/platforms/windows/windows.ps1

	if (!(Test-Path $PSScriptRoot/vcpkg/vcpkg.exe))
	{
		Invoke-Expression("$PSScriptRoot/vcpkg/bootstrap-vcpkg.bat")
	}
}
elseif ($IsMacOS)
{
	& $PSScriptRoot/scripts/platforms/osx/osx.ps1

	if (!(Test-Path $PSScriptRoot/vcpkg/vcpkg))
	{
		Invoke-Expression("sh $PSScriptRoot/vcpkg/bootstrap-vcpkg.sh")
	}
}
elseif ($IsLinux)
{
	& $PSScriptRoot/scripts/platforms/linux/linux.ps1

	if (!(Test-Path $PSScriptRoot/vcpkg/vcpkg))
	{
		Invoke-Expression("sh $PSScriptRoot/vcpkg/bootstrap-vcpkg.sh")
	}
}
else
{
	Write-Error "Unsupported Operating System" # please implement me
	exit
}

$global:myEnv | Add-Member -Force -PassThru -NotePropertyName "VCPKG_ROOT" -NotePropertyValue $("$PSScriptRoot" + [IO.Path]::DirectorySeparatorChar + 'vcpkg') | Out-Null
$global:myEnv | Add-Member -Force -PassThru -NotePropertyName "VCPKG_HOST_TRIPLET" -NotePropertyValue $(Get-HostTriplet) | Out-Null
$global:myEnv | ConvertTo-Json | Out-File $myEnvFile -Force

Read-EnvFile "$PSScriptRoot/.env.json"

$CMakePresets = [ordered] @{
	version = 8
	configurePresets = @(
		[ordered] @{
			name = 'ninja-multi-vcpkg'
			generator = 'Ninja Multi-Config'
			description = 'Configure with vcpkg toolchain and generate Ninja project files for all configurations'
			binaryDir = '${sourceDir}/build/staging/${presetName}'
			toolchainFile = "$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
			installDir = '${sourceDir}/build/install/${presetName}'
			cacheVariables = [ordered] @{
				CMAKE_CONFIGURATION_TYPES = 'debug;profile;release'
				CMAKE_EXPORT_COMPILE_COMMANDS = 'ON'
				CMAKE_MAP_IMPORTED_CONFIG_PROFILE = ';profile;release'
				VCPKG_CHAINLOAD_TOOLCHAIN_FILE = '${sourceDir}/scripts/cmake/toolchains/clang.toolchain.cmake'
				VCPKG_MANIFEST_DIR = '${sourceDir}'
				VCPKG_MANIFEST_FEATURES = 'client;server'
				VCPKG_INSTALLED_DIR = '${sourceDir}/build/packages'
				VCPKG_HOST_TRIPLET = '${presetName}'
				VCPKG_TARGET_TRIPLET = '${presetName}'
				VCPKG_OVERLAY_TRIPLETS = '${sourceDir}/scripts/cmake/triplets'
				VCPKG_VERBOSE = 'ON'
			}
			environment = [ordered] @{
				TOOLS_ROOT = '${sourceDir}/build/packages/${presetName}/tools'
				VCPKG_ROOT = "$env:VCPKG_ROOT"
			}
			warnings = [ordered] @{
				dev = $false
			}
			hidden = $true
		}
		[ordered] @{
			name = 'x64-windows-clang'
			inherits = 'ninja-multi-vcpkg'
			environment = [ordered] @{
				LLVM_ROOT = '${sourceDir}/build/toolchain/x64-windows-release'
				PATH = "`${penv:PATH};`${env:LLVM_ROOT}/bin;`${env:LLVM_ROOT}/tools/llvm;$env:WINDOWS_SDK/bin/$env:WINDOWS_SDK_VERSION/x64"
				VISUAL_STUDIO_PATH = "$env:VISUAL_STUDIO_PATH"
				VISUAL_STUDIO_VCTOOLS_VERSION = "$env:VISUAL_STUDIO_VCTOOLS_VERSION"
			}
			condition = [ordered] @{
				type = 'equals'
				lhs = '${hostSystemName}'
				rhs = 'Windows'
			}
		}
		[ordered] @{
			name = 'arm64-osx-clang'
			inherits = 'ninja-multi-vcpkg'
			environment = [ordered] @{
				LLVM_ROOT = '${sourceDir}/build/toolchain/arm64-osx-release'
				DYLD_LIBRARY_PATH = "`${penv:DYLD_LIBRARY_PATH}:`${env:LLVM_ROOT}/lib"
			}
			cacheVariables = [ordered] @{
				CMAKE_APPLE_SILICON_PROCESSOR = 'arm64'
				CMAKE_OSX_SYSROOT = '/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk'
			}
			condition = [ordered] @{
				type = 'equals'
				lhs = '${hostSystemName}'
				rhs = 'Darwin'
			}
		}
		[ordered] @{
			name = 'arm64-linux-clang'
			inherits = 'ninja-multi-vcpkg'
			environment = [ordered] @{
				LLVM_ROOT = '${sourceDir}/build/toolchain/arm64-linux-release'
				LD_LIBRARY_PATH = "`${penv:LD_LIBRARY_PATH}:`${env:LLVM_ROOT}/lib"
			}
			condition = [ordered] @{
				type = 'equals'
				lhs = '${hostSystemName}'
				rhs = 'Linux'
			}
		}
		[ordered] @{
			name = 'x64-linux-clang'
			inherits = 'ninja-multi-vcpkg'
			environment = [ordered] @{
				LLVM_ROOT = '${sourceDir}/build/toolchain/x64-linux-release'
				LD_LIBRARY_PATH = "`${penv:LD_LIBRARY_PATH}:`${env:LLVM_ROOT}/lib"
			}
			condition = [ordered] @{
				type = 'equals'
				lhs = '${hostSystemName}'
				rhs = 'Linux'
			}
		}
	)
	buildPresets = @(
		[ordered] @{
			name = 'x64-windows'
			configurePreset = 'x64-windows-clang'
			hidden = $true
		}
		[ordered] @{
			name = 'x64-windows-debug'
			configurePreset = 'x64-windows-clang'
			configuration = 'debug'
		}
		[ordered] @{
			name = 'x64-windows-release'
			configurePreset = 'x64-windows-clang'
			configuration = 'release'
		}
		[ordered] @{
			name = 'x64-windows-profile'
			inherits = 'x64-windows-release'
			configuration = 'profile'
		}
		[ordered] @{
			name = 'arm64-osx'
			configurePreset = 'arm64-osx-clang'
			hidden = $true
		}
		[ordered] @{
			name = 'arm64-osx-debug'
			configurePreset = 'arm64-osx-clang'
			configuration = 'debug'
		}
		[ordered] @{
			name = 'arm64-osx-release'
			configurePreset = 'arm64-osx-clang'
			configuration = 'release'
		}
		[ordered] @{
			name = 'arm64-osx-profile'
			inherits = 'arm64-osx-release'
			configuration = 'profile'
		}
		[ordered] @{
			name = 'arm64-linux'
			configurePreset = 'arm64-linux-clang'
			hidden = $true
		}
		[ordered] @{
			name = 'arm64-linux-debug'
			configurePreset = 'arm64-linux-clang'
			configuration = 'debug'
		}
		[ordered] @{
			name = 'arm64-linux-release'
			configurePreset = 'arm64-linux-clang'
			configuration = 'release'
		}
		[ordered] @{
			name = 'arm64-linux-profile'
			inherits = 'arm64-linux-release'
			configuration = 'profile'
		}
		[ordered] @{
			name = 'x64-linux'
			configurePreset = 'x64-linux-clang'
			hidden = $true
		}
		[ordered] @{
			name = 'x64-linux-debug'
			configurePreset = 'x64-linux-clang'
			configuration = 'debug'
		}
		[ordered] @{
			name = 'x64-linux-release'
			configurePreset = 'x64-linux-clang'
			configuration = 'release'
		}
		[ordered] @{
			name = 'x64-linux-profile'
			inherits = 'x64-linux-release'
			configuration = 'profile'
		}
	)
}
$CMakePresets | ConvertTo-Json -Depth 4 | Out-File "$PSScriptRoot/CMakeUserPresets.json" -Force

$VSCodeSettings = [ordered] @{
	'clangd.path' = '${workspaceFolder}/build/toolchain/x64-windows-release/tools/llvm/clangd.exe'
	'clangd.arguments' = @(
		'-log=verbose',
		'-pretty',
		'--background-index',
		'--compile-commands-dir=${workspaceFolder}/build/staging/x64-windows-clang'
	)
}
$VSCodeSettings | ConvertTo-Json -Depth 2 | Out-File "$PSScriptRoot/.vscode/settings.json" -Force

#Invoke-Expression("$PSScriptRoot/vcpkg/vcpkg install --vcpkg-root $env:VCPKG_ROOT --x-install-root=$PSScriptRoot/build/toolchain --overlay-triplets=$PSScriptRoot/scripts/cmake/triplets --triplet $Env:VCPKG_HOST_TRIPLET --x-feature=toolchain --x-abi-tools-use-exact-versions --no-print-usage")
