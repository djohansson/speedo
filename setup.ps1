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

$Configurations = @('debug', 'release', 'profile')
$Targets = @('client', 'server')

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
				CMAKE_MAP_IMPORTED_CONFIG_PROFILE = 'profile;release'
				#CMAKE_FASTBUILD_VERBOSE_GENERATOR = 'ON' # docs say this var exists, but we get unused variable warnings with it defined.
				#CMAKE_FASTBUILD_ALLOW_RESPONSE_FILES = 'ON' # docs say this var exists, but we get unused variable warnings with it defined.
				CMAKE_FASTBUILD_FORCE_RESPONSE_FILE = 'OFF'
				#CMAKE_FASTBUILD_TRACK_BYPRODUCTS_AS_OUTPUTS = 'OFF' # docs say this var exists, but we get unused variable warnings with it defined.
				CMAKE_FASTBUILD_CLANG_REWRITE_INCLUDES = 'OFF' # disabled due to issues with some c-libraries, cargs for example.
				CMAKE_FASTBUILD_USE_DETERMINISTIC_PATHS = 'ON'
				CMAKE_FASTBUILD_USE_LIGHTCACHE = 'ON'
				CMAKE_FASTBUILD_USE_RELATIVE_PATHS = 'ON'
				VCPKG_CHAINLOAD_TOOLCHAIN_FILE = '${sourceDir}/scripts/cmake/toolchains/clang.toolchain.cmake' # needs to be duplicated here to be work in the vscode cmake extension
				VCPKG_OVERLAY_TRIPLETS = '${sourceDir}/scripts/cmake/triplets'
				VCPKG_MANIFEST_DIR = '${sourceDir}'
				VCPKG_VERBOSE = 'ON'
			}
			environment = [ordered] @{
				VCPKG_ROOT = "$env:VCPKG_ROOT"
				FASTBUILD_TEMP_PATH = '${sourceDir}/temp' # dont use user/machine specific temp paths, keep it local to the source tree to not mess with other builds on the same machine and to be able to easily clean it up.
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
			name = 'llvm-build'
			hidden = $true
			inherits = 'vcpkg'
			binaryDir = "`${sourceDir}/build/`${presetName}"
			installDir = "`${sourceDir}/install/`${presetName}"
			cacheVariables = [ordered] @{
				VCPKG_HOST_TRIPLET = "$(Get-HostTriplet)"
				VCPKG_TARGET_TRIPLET = "$(Get-TargetTriplet)"
				VCPKG_INSTALL_OPTIONS = '--x-abi-tools-use-exact-versions;--no-print-usage' #;--debug'
			}
			environment = [ordered] @{
				LLVM_ROOT = "`${sourceDir}/install/$(Get-HostTriplet)"
				LLVM_TOOLS_BINARY_DIR = "`${sourceDir}/install/$(Get-HostTriplet)/tools/llvm"
			}
		}
	)
	buildPresets = @()
}

$VSCodeLaunchConfiguration = [ordered] @{
	'version' = '0.2.1'
	'configurations' = @()
	'compounds' = @()
}

$VSCodeTasks = @{
	version = '2.0.0'
	tasks = @()
}

if ($IsWindows)
{
	foreach ($Config in $Configurations)
	{
		$CMakePresets.configurePresets += @(
			[ordered] @{
				name = "$(Get-TargetTriplet)-$Config"
				inherits = 'llvm-build'
				environment = [ordered] @{
					PATH = "`$env{LLVM_ROOT}/bin`;`${sourceDir}/install/$(Get-TargetTriplet)/tools/mimalloc"
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

		foreach ($Target in $Targets)
		{
			$VSCodeLaunchConfiguration.configurations += @(
				[ordered] @{
					name = "($(Get-TargetTriplet)) $Config/$Target"
					type = "cppvsdbg"
					request = "launch"
					program = "`${workspaceFolder}/build/$(Get-TargetTriplet)-$Config/$Target.exe"
					args = @("-u `${userHome}/.speedo")
					cwd = "`${workspaceFolder}"
					console = "internalConsole"
					environment = @(
						@{ "name" = "MIMALLOC_SHOW_STATS"; "value" = "$($Config -eq 'debug' ? '1' : '0')" },
						@{ "name" = "MIMALLOC_VERBOSE"; "value" = "$($Config -eq 'debug' ? '1' : '0')" },
						@{ "name" = "MIMALLOC_SHOW_ERRORS"; "value" = "$($Config -eq 'debug' ? '1' : '0')" },
						@{ "name" = "PATH"; "value" = "`${workspaceFolder}/install/$(Get-TargetTriplet)/$($Config -eq 'debug' ? 'debug/bin' : 'bin')" },
						@{ "name" = "VK_LAYER_PATH"; "value" = "`${workspaceFolder}/install/$(Get-TargetTriplet)/$($Config -eq 'debug' ? 'debug/bin' : 'bin')" }
					)
					symbolOptions = @{
						searchPaths = @()
						searchMicrosoftSymbolServer = $true
						cachePath = "${env:TEMP}/symbolcache"
						moduleFilter = @{
							mode = "loadAllButExcluded"
							excludedModules = @()
						}
					}
				}
			)
		}
	}
}
elseif ($IsMacOS)
{
	foreach ($Config in $Configurations)
	{
		$CMakePresets.configurePresets += @(
			[ordered] @{
				name = "$(Get-TargetTriplet)-$Config"
				inherits = 'llvm-build'
				cacheVariables = [ordered] @{
					CMAKE_APPLE_SILICON_PROCESSOR = "$(Get-TargetArchitecture)"
					CMAKE_OSX_SYSROOT = "$env:MACOS_SDK_PATH"
					VCPKG_OSX_ARCHITECTURES = "$(Get-TargetArchitecture)"
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

		foreach ($Target in $Targets)
		{
			$VSCodeLaunchConfiguration.configurations += @(
				[ordered] @{
					name = "($(Get-TargetTriplet)) $Config/$Target"
					type = "lldb"
					request = "launch"
					program = "`${workspaceFolder}/build/$(Get-TargetTriplet)-$Config/$Target"
					args = @("-u `${userHome}/.speedo")
					cwd = "`${workspaceFolder}"
					terminal = "console"
					preLaunchTask = "export-kosmickrisp-driver-path"
					envFile = "`${userHome}/.speedo/.env"
					env = @{
						"MIMALLOC_SHOW_STATS" = "$($Config -eq 'debug' ? '1' : '0')"
						"MIMALLOC_VERBOSE" = "$($Config -eq 'debug' ? '1' : '0')"
						"MIMALLOC_SHOW_ERRORS" = "$($Config -eq 'debug' ? '1' : '0')"
						"DYLD_LIBRARY_PATH" = "`${workspaceFolder}/install/$(Get-TargetTriplet)/$($Config -eq 'debug' ? 'debug/lib' : 'lib')"
						"DYLD_INSERT_LIBRARIES" = "libmimalloc-secure$($Config -eq 'debug' ? '-debug' : '') .dylib"
						"DYLD_PRINT_LIBRARIES" = "$($Config -eq 'debug' ? '1' : '0')"
						"TSAN_OPTIONS" = "suppressions=`${workspaceFolder}/tsan-suppressions.txt"
						"VK_LAYER_PATH" = "`${workspaceFolder}/install/$(Get-TargetTriplet)/share/vulkan/explicit_layer.d"
					}
				}
			)
		}
	}

	$VSCodeTasks.tasks += @(
		[ordered] @{
			label = "export-molten-vk-driver-path"
			type = "shell"
			command = "pwsh -c '& { if (-not (Test-Path `${userHome}/.speedo)) { New-Item -Force -ItemType Directory -Path `${userHome}/.speedo | Out-Null }; `"VK_DRIVER_FILES=`$(brew --prefix molten-vk)/etc/vulkan/icd.d/MoltenVK_icd.json`" > `${userHome}/.speedo/.env }' 2>&1"
			group = "none"
		},
		[ordered] @{
			label = "export-kosmickrisp-driver-path"
			type = "shell"
			command = "pwsh -c '& { if (-not (Test-Path `${userHome}/.speedo)) { New-Item -Force -ItemType Directory -Path `${userHome}/.speedo | Out-Null }; `"VK_DRIVER_FILES=`$(brew --prefix mesa)/share/vulkan/icd.d/kosmickrisp_mesa_icd.aarch64.json`" > `${userHome}/.speedo/.env }' 2>&1"
			group = "none"
		}
	)
}
elseif ($IsLinux)
{
	foreach ($Config in $Configurations)
	{
		$CMakePresets.configurePresets += @(
			[ordered] @{
				name = "$(Get-TargetTriplet)-$Config"
				inherits = 'llvm-build'
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

		foreach ($Target in $Targets)
		{
			$VSCodeLaunchConfiguration.configurations += @(
				[ordered] @{
					name = "($(Get-TargetTriplet)) $Config/$Target"
					type = "lldb"
					request = "launch"
					program = "`${workspaceFolder}/build/$(Get-TargetTriplet)-$Config/$Target"
					args = @("-u `${userHome}/.speedo")
					cwd = "`${workspaceFolder}"
					terminal = "console"
					envFile = "`${userHome}/.speedo/.env"
					env = @{
						"MIMALLOC_SHOW_STATS" = "$($Config -eq 'debug' ? '1' : '0')"
						"MIMALLOC_VERBOSE" = "$($Config -eq 'debug' ? '1' : '0')"
						"MIMALLOC_SHOW_ERRORS" = "$($Config -eq 'debug' ? '1' : '0')"
						"LD_LIBRARY_PATH" = "`${workspaceFolder}/install/$(Get-TargetTriplet)/$($Config -eq 'debug' ? 'debug/lib' : 'lib')"
						"VK_LAYER_PATH" = "`${workspaceFolder}/install/$(Get-TargetTriplet)/share/vulkan/explicit_layer.d"
					}
				}
			)
		}
	}
}

foreach ($Config in $Configurations)
{
	$CMakePresets.buildPresets += @(
		[ordered] @{
			name = "$(Get-TargetTriplet)-$Config"
			configurePreset = "$(Get-TargetTriplet)-$Config"
			configuration = $Config
		}
	)

	$VSCodeLaunchConfiguration.compounds += @(
		[ordered] @{
			name = "($(Get-TargetTriplet)) $Config all"
			configurations = @(
				"($(Get-TargetTriplet)) $Config/client",
				"($(Get-TargetTriplet)) $Config/server"
			)
			stopAll = $true
		}
	)

	$VSCodeTasks.tasks += @(
		[ordered] @{
			label = "Clang-Format ($Config)"
			type = "shell"
			command = "pwsh -c '& {. `${workspaceFolder}/scripts/env.ps1; Initialize-ToolchainEnv; (Get-ChildItem -Path `${workspaceFolder}/src -Include *.h,*.c,*.cpp -Recurs -File).FullName | Out-File -FilePath `${workspaceFolder}/build/$(Get-TargetTriplet)-$Config/.clang-format-files -Force; & `$env:LLVM_TOOLS_PATH/clang-format -i -style=file -files `${workspaceFolder}/build/$(Get-TargetTriplet)-$Config/.clang-format-files } 2>&1'"
			group = "none"
		},
		[ordered] @{
			label = "Clang-Tidy ($Config)"
			type = "shell"
			command = "pwsh -c '& {. `${workspaceFolder}/scripts/env.ps1; Initialize-ToolchainEnv; & `$env:LLVM_TOOLS_PATH/clang-tidy --fix --format-style=file -p `${workspaceFolder}/build/$(Get-TargetTriplet)-$Config/compile_commands.json } 2>&1'"
			group = "none"
		}
	)
}

# $VSCodeTasks.tasks += @(
# 	[ordered] @{
# 		type = "cmake"
# 		label = "CMake: build (All)"
# 		command = "build"
# 		targets = @(
# 			"all"
# 		)
# 		preset = '${command:cmake.activeBuildPresetName}'
# 		group = @{
# 			kind = "build"
# 			isDefault = $true
# 		}
# 		problemMatcher = @()
# 		detail = "CMake template build task"
# 	}
# )

$VSCodeSettings = [ordered] @{
	'clangd.path' = "`${workspaceFolder}/install/$(Get-HostTriplet)/tools/llvm/clangd$($IsWindows ? '.exe' : '')"
	'clangd.arguments' = @(
		'-log=verbose',
		'-pretty',
		'--background-index',
		"--compile-commands-dir=`${workspaceFolder}/install/$(Get-TargetTriplet)-debug"
	)
	'cmake.cmakePath' = "`${workspaceFolder}/vcpkg/downloads/tools/cmake-4.2.3-$(Get-HostOS)/cmake-4.2.3-$($IsWindows ? 'windows-x86_64' : ($IsMacOS ? 'macos-arm64' : "linux-$(Get-HostArchitecture)"))/bin/cmake$($IsWindows ? '.exe' : '')"
}

$CMakePresets | ConvertTo-Json -Depth 4 | Out-File "$PSScriptRoot/CMakeUserPresets.json" -Force
$VSCodeLaunchConfiguration | ConvertTo-Json -Depth 5 | Out-File "$PSScriptRoot/.vscode/launch.json" -Force
$VSCodeSettings | ConvertTo-Json -Depth 2 | Out-File "$PSScriptRoot/.vscode/settings.json" -Force
$VSCodeTasks | ConvertTo-Json -Depth 2 | Out-File "$PSScriptRoot/.vscode/tasks.json" -Force

$env:LLVM_ROOT = "$PSScriptRoot/install/$(Get-HostTriplet)"
$env:LLVM_TOOLS_BINARY_DIR = "$PSScriptRoot/install/$(Get-HostTriplet)/tools/llvm"

$VcpkgOptions = @(
	"--vcpkg-root $env:VCPKG_ROOT"
	"--x-install-root=$PSScriptRoot/install"
	"--overlay-triplets=$PSScriptRoot/scripts/cmake/triplets"
	"--host-triplet $(Get-HostTriplet)"
	"--triplet=$(Get-TargetTriplet)"
	'--x-abi-tools-use-exact-versions'
	'--no-print-usage'
) -join ' '

Invoke-Expression("$PSScriptRoot/vcpkg/vcpkg install $VcpkgOptions")
