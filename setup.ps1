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
$global:myEnv | ConvertTo-Json | Out-File $myEnvFile -Force

Read-EnvFile "$PSScriptRoot/.env.json"

$CMakePresets = [ordered] @{
	version = 8
	configurePresets = @(
		[ordered] @{
			name = 'vcpkg'
			hidden = $true
			generator = 'FASTBuild'
			description = 'Configure with vcpkg toolchain and generate project files for all configurations'
			toolchainFile = "$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
			cacheVariables = [ordered] @{
				CMAKE_EXPORT_COMPILE_COMMANDS = 'ON'
				CMAKE_FASTBUILD_VERBOSE_GENERATOR = 'ON'
				CMAKE_FASTBUILD_ALLOW_RESPONSE_FILES = 'ON'
				CMAKE_FASTBUILD_FORCE_RESPONSE_FILE = 'OFF'
				CMAKE_FASTBUILD_TRACK_BYPRODUCTS_AS_OUTPUTS = 'OFF'
				CMAKE_FASTBUILD_CLANG_REWRITE_INCLUDES = 'OFF' # disabled due to issues with some c-libraries, cargs for example.
				CMAKE_FASTBUILD_USE_DETERMINISTIC_PATHS = 'ON'
				CMAKE_FASTBUILD_USE_LIGHTCACHE = 'ON'
				CMAKE_FASTBUILD_USE_RELATIVE_PATHS = 'ON'
				VCPKG_OVERLAY_TRIPLETS = '${sourceDir}/scripts/cmake/triplets'
				VCPKG_MANIFEST_DIR = '${sourceDir}'
				VCPKG_VERBOSE = 'ON'
			}
			environment = [ordered] @{
				VCPKG_ROOT = "$env:VCPKG_ROOT"
				FASTBUILD_TEMP_PATH = '${sourceDir}/build/temp' # dont use user/machine specific temp paths, keep it local to the source tree to not mess with other builds on the same machine and to be able to easily clean it up.
				FASTBUILD_BROKERAGE_PATH = "`$penv{FASTBUILD_BROKERAGE_PATH}"
				FASTBUILD_WORKERS = "`$penv{FASTBUILD_WORKERS}"
				FASTBUILD_CACHE_PATH = "`$penv{FASTBUILD_CACHE_PATH}"
				FASTBUILD_CACHE_PATH_MOUNT_POINT = "`$penv{FASTBUILD_CACHE_PATH_MOUNT_POINT}"
				FASTBUILD_CACHE_MODE = "`$penv{FASTBUILD_CACHE_MODE}"
			}
			warnings = [ordered] @{
				dev = $false
			}
		}
		[ordered] @{
			name = 'build'
			hidden = $true
			inherits = 'vcpkg'
			binaryDir = "`${sourceDir}/build/cmake"
			installDir = "`${sourceDir}/build/install"
			cacheVariables = [ordered] @{
				CMAKE_CONFIGURATION_TYPES = 'debug;profile;release'
				CMAKE_MAP_IMPORTED_CONFIG_PROFILE = ';profile;release'
				VCPKG_HOST_TRIPLET = "$(Get-HostTriplet)"
				VCPKG_TARGET_TRIPLET = "$(Get-TargetTriplet)"
				VCPKG_INSTALLED_DIR = "`${sourceDir}/build/install"
				VCPKG_INSTALL_OPTIONS = '--x-abi-tools-use-exact-versions;--no-print-usage'
				VCPKG_DISABLE_COMPILER_TRACKING = 'ON'
				#VCPKG_KEEP_ENV_VARS = '' # dont use: https://github.com/microsoft/vcpkg/discussions/42064
			}
			environment = [ordered] @{
				LLVM_ROOT = "`${sourceDir}/build/install/$(Get-HostTriplet)"
				LLVM_TOOLS_BINARY_DIR = "`${sourceDir}/build/install/$(Get-HostTriplet)/tools/llvm"
			}
		}
	)
	buildPresets = @()
}

if ($IsWindows)
{
	$CMakePresets.configurePresets += @(
		[ordered] @{
			name = "build-$(Get-TargetTuplet)"
			inherits = 'build'
			cacheVariables = [ordered] @{
				VCPKG_VISUAL_STUDIO_PATH = "$VSPath"
			}
			environment = [ordered] @{
				PATH = "`$penv{PATH};`$env{LLVM_ROOT}/bin"
				VISUAL_STUDIO_PATH = "$env:VISUAL_STUDIO_PATH"
				VISUAL_STUDIO_VCTOOLS_VERSION = "$env:VISUAL_STUDIO_VCTOOLS_VERSION"
				WINDOWS_SDK_PATH = "$env:WINDOWS_SDK"
				WINDOWS_SDK_VERSION = "$env:WINDOWS_SDK_VERSION"
			}
			condition = [ordered] @{
				type = 'equals'
				lhs = '${hostSystemName}'
				rhs = 'Windows'
			}
		}
	)
}
elseif ($IsMacOS)
{
	$CMakePresets.configurePresets += @(
		[ordered] @{
			name = "build-$(Get-TargetTuplet)"
			inherits = 'build'
			cacheVariables = [ordered] @{
				CMAKE_APPLE_SILICON_PROCESSOR = "$(Get-HostArchitecture)"
				CMAKE_OSX_SYSROOT = "$env:MACOS_SDK_PATH"
				VCPKG_OSX_ARCHITECTURES = "$(Get-HostArchitecture)"
				VCPKG_OSX_SYSROOT = "$env:MACOS_SDK_PATH"
				VCPKG_FIXUP_ELF_RPATH = 'ON'
			}
			environment = [ordered] @{
				DYLD_LIBRARY_PATH = "`$penv{DYLD_LIBRARY_PATH}:`$env{LLVM_ROOT}/lib"
			}
			condition = [ordered] @{
				type = 'equals'
				lhs = '${hostSystemName}'
				rhs = 'Darwin'
			}
		}
	)
}
elseif ($IsLinux)
{
	$CMakePresets.configurePresets += @(
		[ordered] @{
			name = "build-$(Get-TargetTuplet)"
			inherits = 'build'
			environment = [ordered] @{
				LD_LIBRARY_PATH = "`$penv{LD_LIBRARY_PATH}:`$env{LLVM_ROOT}/lib"
			}
			condition = [ordered] @{
				type = 'equals'
				lhs = '${hostSystemName}'
				rhs = 'Linux'
			}
		}
	)
}

$CMakePresets.buildPresets += @(
	[ordered] @{
		name = "build-$(Get-TargetTuplet)"
		hidden = $true
		configurePreset = "build-$(Get-TargetTuplet)"
	}
	[ordered] @{
		name = "$(Get-TargetTuplet)-debug"
		inherits = "build-$(Get-TargetTuplet)"
		configuration = 'debug'
	}
	[ordered] @{
		name = "$(Get-TargetTuplet)-release"
		inherits = "build-$(Get-TargetTuplet)"
		configuration = 'release'
	}
	[ordered] @{
		name = "$(Get-TargetTuplet)-profile"
		inherits = "$(Get-TargetTuplet)-release"
		configuration = 'profile'
	}
)

$CMakePresets | ConvertTo-Json -Depth 4 | Out-File "$PSScriptRoot/CMakeUserPresets.json" -Force

$VSCodeSettings = [ordered] @{
	'clangd.path' = "`${workspaceFolder}/build/host-toolchain-install/$(Get-HostTriplet)/tools/llvm/clangd$($IsWindows ? '.exe' : '')"
	'clangd.arguments' = @(
		'-log=verbose',
		'-pretty',
		'--background-index',
		"--compile-commands-dir=`${workspaceFolder}/build/target-toolchain-install/$(Get-TargetTuplet)-debug"
	)
	'cmake.cmakePath' = "`${workspaceFolder}/vcpkg/downloads/tools/cmake-4.2.3-$(Get-HostOS)/cmake-4.2.3-$($IsWindows ? 'windows-x86_64' : ($IsMacOS ? 'macos-arm64' : "linux-$(Get-HostArchitecture)"))/bin/cmake$($IsWindows ? '.exe' : '')"
}

$VSCodeSettings | ConvertTo-Json -Depth 2 | Out-File "$PSScriptRoot/.vscode/settings.json" -Force

$env:LLVM_ROOT = "$PSScriptRoot/build/install/$(Get-HostTriplet)"
$env:LLVM_TOOLS_BINARY_DIR = "$PSScriptRoot/build/install/$(Get-HostTriplet)/tools/llvm"

$VcpkgOptions = @(
	"--vcpkg-root $env:VCPKG_ROOT"
	"--x-install-root=$PSScriptRoot/build/install"
	"--overlay-triplets=$PSScriptRoot/scripts/cmake/triplets"
	"--host-triplet $(Get-HostTriplet)"
	"--triplet=$(Get-TargetTriplet)"
	'--x-abi-tools-use-exact-versions'
	'--no-print-usage'
) -join ' '

Invoke-Expression("$PSScriptRoot/vcpkg/vcpkg install $VcpkgOptions")
