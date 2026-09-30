# Make Write-Error and cmdlet errors fatal (exit code 1). Native commands don't throw on non-zero exit codes
# (some, like `brew list` and `xcode-select --install`, are expected to fail), so check $LASTEXITCODE explicitly.
$ErrorActionPreference = 'Stop'

# the setup_<platform> bootstrappers install/upgrade PowerShell before running this script; fail when launched directly on an older one
if ($PSVersionTable.PSVersion -lt [System.Version]"7.5")
{
	$Bootstrapper = $IsWindows ? "setup_windows.bat" : ($IsMacOS ? "setup_osx.sh" : "setup_linux.sh")
	Write-Error "PowerShell $($PSVersionTable.PSVersion) is older than the required 7.5. Run $Bootstrapper instead, which upgrades it and then runs setup."
}

. $PSScriptRoot/scripts/env.ps1
. $PSScriptRoot/scripts/platform.ps1

$EnvFile = "$PSScriptRoot/.env.json"
# always a hashtable, assign with $global:Env['KEY'] = ... (Add-Member on a hashtable is not serialized by ConvertTo-Json)
$global:Env = [ordered]@{}
if (Test-Path $EnvFile)
{
	$global:Env = Get-Content -Path $EnvFile -Raw | ConvertFrom-Json -AsHashtable
}

# values that, when changed by an OS/SDK/toolchain update, invalidate existing CMake build directories
$PlatformStateKeys = @('SDKROOT', 'MACOS_BUILD_VERSION', 'WINDOWS_SDK_VERSION', 'VISUAL_STUDIO_VCTOOLS_VERSION')
$PreviousPlatformState = @{}
foreach ($Key in $PlatformStateKeys)
{
	$PreviousPlatformState[$Key] = $global:Env[$Key]
}

$PlatformIndex = (!(Test-Path Variable:\IsWindows) -or $IsWindows) ? 0 : $IsMacOS ? 1 : $IsLinux ? 2 : -1
$Platforms = @('windows', 'osx', 'linux')

if ($PlatformIndex -ge 0 -and $PlatformIndex -lt $Platforms.Length)
{
	$exeSuffix = $IsWindows ? '.exe' : ''
	$scriptSuffix = $IsWindows ? '.bat' : '.sh'
	$Platform = $Platforms[$PlatformIndex]
	& $PSScriptRoot/scripts/platforms/$Platform/$Platform.ps1

	if (!(Test-Path $PSScriptRoot/vcpkg/vcpkg$exeSuffix))
	{
		Invoke-Expression("$PSScriptRoot/vcpkg/bootstrap-vcpkg$scriptSuffix")
		if ($LASTEXITCODE -ne 0) { throw "bootstrap-vcpkg$scriptSuffix failed with exit code $LASTEXITCODE" }
	}
}
else
{
	Write-Error "Unsupported Operating System" # please implement me
}

$global:Env['VCPKG_ROOT'] = "$PSScriptRoot" + [IO.Path]::DirectorySeparatorChar + 'vcpkg'
$global:Env | ConvertTo-Json | Out-File $EnvFile -Force

$ChangedPlatformState = $PlatformStateKeys | Where-Object {
	$PreviousPlatformState[$_] -and ($PreviousPlatformState[$_] -ne $global:Env[$_])
}
if ($ChangedPlatformState)
{
	foreach ($Key in $ChangedPlatformState)
	{
		Write-Host "Platform change detected: $Key '$($PreviousPlatformState[$Key])' -> '$($global:Env[$Key])'"
	}
	Write-Host "Clearing CMake caches so compilers and SDK are re-detected..."
	foreach ($BuildDir in Get-ChildItem -Path "$PSScriptRoot/build" -Directory -ErrorAction SilentlyContinue)
	{
		Remove-Item -Path "$BuildDir/CMakeCache.txt", "$BuildDir/CMakeFiles" -Recurse -Force -ErrorAction SilentlyContinue
	}
}

Read-EnvFile $EnvFile

$Configurations = @('debug', 'release', 'profile')
$Targets = @('client', 'server')

$CMakePresets = [ordered] @{
	version = 8
	configurePresets = @(
		[ordered] @{
			name = 'vcpkg'
			hidden = $true
			description = 'Configure with vcpkg and generate project files for all configurations'
			toolchainFile = "$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
			cacheVariables = [ordered] @{
				VCPKG_HOST_TRIPLET = "$(Get-HostTriplet)"
				VCPKG_TARGET_TRIPLET = "$(Get-TargetTriplet)"
				VCPKG_INSTALL_OPTIONS = '--x-abi-tools-use-exact-versions;--no-print-usage' #;--debug'
				VCPKG_INSTALLED_DIR = "`${sourceDir}/install" # reminder: installDir (cmake), VCPKG_INSTALLED_DIR (vcpkg, same as --x-install-root)
				VCPKG_OVERLAY_TRIPLETS = '${sourceDir}/scripts/cmake/triplets'
				VCPKG_MANIFEST_DIR = '${sourceDir}'
				# needs to be duplicated here to be work in the vscode cmake extension
				VCPKG_DISABLE_COMPILER_TRACKING = 'ON' # This target is not compiled yet when vcpkg wants to calculate the compiler hash.
				CMAKE_EXPORT_COMPILE_COMMANDS = 'ON'
				CMAKE_MAP_IMPORTED_CONFIG_PROFILE = 'profile;release'
				CMAKE_FASTBUILD_USE_DETERMINISTIC_PATHS = 'ON'
				CMAKE_FASTBUILD_USE_LIGHTCACHE='ON'
				#
			}
			environment = [ordered] @{
				VCPKG_ROOT = "$env:VCPKG_ROOT"
				FASTBUILD_TEMP_PATH = '${sourceDir}/temp' # dont use user/machine specific temp paths, keep it local to the source tree to not mess with other builds on the same machine and to be able to easily clean it up.
				FASTBUILD_BROKERAGE_PATH = $env:FASTBUILD_BROKERAGE_PATH ?? "$HOME/.fastbuild/brokerage"
				FASTBUILD_CACHE_PATH = $env:FASTBUILD_CACHE_PATH ?? "$HOME/.fastbuild/cache"
				FASTBUILD_CACHE_PATH_MOUNT_POINT = $env:FASTBUILD_CACHE_PATH_MOUNT_POINT ?? "false"
				FASTBUILD_CACHE_MODE = $env:FASTBUILD_CACHE_MODE ?? "rw"
			}
			warnings = [ordered] @{
				dev = $false
			}
		}
		[ordered] @{
			name = 'llvm-build'
			hidden = $true
			inherits = 'vcpkg'
			generator = 'FASTBuild'
			binaryDir = "`${sourceDir}/build/`${presetName}"
			installDir = "`${sourceDir}/install/$(Get-TargetTriplet)"
			cacheVariables = [ordered] @{
				# needs to be duplicated here to be work in the vscode cmake extension
				VCPKG_CHAINLOAD_TOOLCHAIN_FILE = '${sourceDir}/scripts/cmake/toolchains/clang.toolchain.cmake'
				#
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

# per-platform settings, indexed by $PlatformIndex (same order as $Platforms)
$PlatformSettings = @(
	@{ # windows
		HostSystemName = 'Windows'
		PresetCacheVariables = [ordered] @{}
		PresetEnvironment = [ordered] @{
			PATH ="`$env{LLVM_ROOT}/bin`;`$env{LLVM_TOOLS_BINARY_DIR}`;`${sourceDir}/install/$(Get-TargetTriplet)/tools/mimalloc`;`$penv{PATH}"
			VISUAL_STUDIO_PATH = "$env:VISUAL_STUDIO_PATH"
			VISUAL_STUDIO_VCTOOLS_VERSION = "$env:VISUAL_STUDIO_VCTOOLS_VERSION"
			WINDOWS_SDK_PATH = "$env:WINDOWS_SDK"
			WINDOWS_SDK_VERSION = "$env:WINDOWS_SDK_VERSION"
		}
		Debugger = 'cppvsdbg'
		LaunchEnvironment = {
			param($Config)
			[ordered] @{
				PATH = "`${workspaceFolder}/install/$(Get-TargetTriplet)/$($Config -eq 'debug' ? 'debug/bin' : 'bin')"
				VK_LAYER_PATH = "`${workspaceFolder}/install/$(Get-TargetTriplet)/$($Config -eq 'debug' ? 'debug/bin' : 'bin')"
			}
		}
		LaunchOptions = {
			param($LaunchEnvironment)
			[ordered] @{
				console = "internalConsole"
				environment = @($LaunchEnvironment.GetEnumerator() | ForEach-Object { @{ "name" = $_.Key; "value" = $_.Value } })
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
		}
		ProfilerLaunchOptions = [ordered] @{
			environment = @(
				@{ "name" = "PATH"; "value" = "`${workspaceFolder}/install/$(Get-TargetTriplet)/bin" }
			)
		}
		Tasks = @()
	}
	@{ # osx
		HostSystemName = 'Darwin'
		PresetCacheVariables = [ordered] @{
			# needs to be duplicated here to be work in the vscode cmake extension
			VCPKG_OSX_SYSROOT = "$env:SDKROOT"
			VCPKG_OSX_ARCHITECTURES = "$env:CMAKE_APPLE_SILICON_PROCESSOR"
			VCPKG_FIXUP_ELF_RPATH = 'ON'
			#
		}
		PresetEnvironment = [ordered] @{
			SDKROOT = "$env:SDKROOT"
			CMAKE_APPLE_SILICON_PROCESSOR = $env:CMAKE_APPLE_SILICON_PROCESSOR ?? 'arm64'
			CMAKE_OSX_ARCHITECTURES = $env:CMAKE_APPLE_SILICON_PROCESSOR ?? 'arm64'
		}
		Debugger = 'lldb'
		LaunchEnvironment = {
			param($Config)
			[ordered] @{
				DYLD_LIBRARY_PATH = "`${workspaceFolder}/install/$(Get-TargetTriplet)/$($Config -eq 'debug' ? 'debug/lib' : 'lib')"
				#DYLD_INSERT_LIBRARIES = "libmimalloc$($Config -eq 'debug' ? '-secure-debug' : '').dylib"
				DYLD_PRINT_LIBRARIES = "$($Config -eq 'debug' ? '1' : '0')"
				TSAN_OPTIONS = "suppressions=`${workspaceFolder}/tsan-suppressions.txt"
				VK_LAYER_PATH = "`${workspaceFolder}/install/$(Get-TargetTriplet)/share/vulkan/explicit_layer.d"
			}
		}
		LaunchOptions = {
			param($LaunchEnvironment)
			[ordered] @{
				terminal = "console"
				preLaunchTask = "export-kosmickrisp-driver-path"
				envFile = "`${userHome}/.speedo/.env"
				env = $LaunchEnvironment
			}
		}
		ProfilerLaunchOptions = [ordered] @{}
		Tasks = @(
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
	@{ # linux
		HostSystemName = 'Linux'
		PresetCacheVariables = [ordered] @{}
		PresetEnvironment = [ordered] @{
			LD_LIBRARY_PATH = "`$penv{LD_LIBRARY_PATH}:`$env{LLVM_ROOT}/lib"
		}
		Debugger = 'lldb'
		LaunchEnvironment = {
			param($Config)
			[ordered] @{
				LD_LIBRARY_PATH = "`${workspaceFolder}/install/$(Get-TargetTriplet)/$($Config -eq 'debug' ? 'debug/lib' : 'lib')"
				VK_LAYER_PATH = "`${workspaceFolder}/install/$(Get-TargetTriplet)/share/vulkan/explicit_layer.d"
			}
		}
		LaunchOptions = {
			param($LaunchEnvironment)
			[ordered] @{
				terminal = "console"
				envFile = "`${userHome}/.speedo/.env"
				env = $LaunchEnvironment
			}
		}
		ProfilerLaunchOptions = [ordered] @{}
		Tasks = @()
	}
)

if ($PlatformIndex -ge 0 -and $PlatformIndex -lt $PlatformSettings.Length)
{
	$Settings = $PlatformSettings[$PlatformIndex]

	foreach ($Config in $Configurations)
	{
		$CacheVariables = [ordered] @{
			CMAKE_BUILD_TYPE = $Config # FASTBuild is a single-config generator
		}
		foreach ($Entry in $Settings.PresetCacheVariables.GetEnumerator()) { $CacheVariables[$Entry.Key] = $Entry.Value }

		$CMakePresets.configurePresets += @(
			[ordered] @{
				name = "$(Get-TargetTriplet)-$Config"
				inherits = 'llvm-build'
				cacheVariables = $CacheVariables
				environment = $Settings.PresetEnvironment
				condition = [ordered] @{
					type = 'equals'
					lhs = '${hostSystemName}'
					rhs = $Settings.HostSystemName
				}
			}
		)

		$LaunchEnvironment = [ordered] @{
			SPEEDO_AUTOLOAD_MODEL = "gallery.obj"
			SPEEDO_AUTOLOAD_IMAGE = "gallery.jpg"
			MIMALLOC_SHOW_STATS = "$($Config -eq 'debug' ? '1' : '0')"
			MIMALLOC_VERBOSE = "$($Config -eq 'debug' ? '1' : '0')"
			MIMALLOC_SHOW_ERRORS = "$($Config -eq 'debug' ? '1' : '0')"
		}
		foreach ($Entry in (& $Settings.LaunchEnvironment $Config).GetEnumerator()) { $LaunchEnvironment[$Entry.Key] = $Entry.Value }

		foreach ($Target in $Targets)
		{
			$LaunchConfiguration = [ordered] @{
				name = "($(Get-TargetTriplet)) $Config/$Target"
				type = $Settings.Debugger
				request = "launch"
				program = "`${workspaceFolder}/build/$(Get-TargetTriplet)-$Config/$Target$exeSuffix"
				args = @("-u `${userHome}/.speedo")
				cwd = "`${workspaceFolder}"
			}
			foreach ($Entry in (& $Settings.LaunchOptions $LaunchEnvironment).GetEnumerator()) { $LaunchConfiguration[$Entry.Key] = $Entry.Value }

			$VSCodeLaunchConfiguration.configurations += @($LaunchConfiguration)
		}
	}

	# tracy[gui-tools] only installs a release build of the profiler, shared by all configurations
	$VSCodeLaunchConfiguration.configurations += @(
		[ordered] @{
			name = "($(Get-TargetTriplet)) tracy-profiler"
			type = $Settings.Debugger
			request = "launch"
			program = "`${workspaceFolder}/install/$(Get-TargetTriplet)/tools/tracy/tracy-profiler$exeSuffix"
			cwd = "`${workspaceFolder}"
		}
	)
	foreach ($Entry in $Settings.ProfilerLaunchOptions.GetEnumerator()) { $VSCodeLaunchConfiguration.configurations[-1][$Entry.Key] = $Entry.Value }

	$VSCodeTasks.tasks += $Settings.Tasks
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
				"($(Get-TargetTriplet)) $Config/server",
				"($(Get-TargetTriplet)) tracy-profiler"
			)
			stopAll = $true
		}
	)

	$VSCodeTasks.tasks += @(
		[ordered] @{
			label = "Clang-Format ($Config)"
			type = "shell"
			command = "pwsh -c '& {. `${workspaceFolder}/scripts/env.ps1; Initialize-ToolchainEnv; (Get-ChildItem -Path `${workspaceFolder}/src -Include *.h,*.inl,*.c,*.cpp -Recurs -File).FullName | Out-File -FilePath `${workspaceFolder}/build/$(Get-TargetTriplet)-$Config/.clang-format-files -Force; & `$env:LLVM_TOOLS_BINARY_DIR/clang-format -i -style=file -files `${workspaceFolder}/build/$(Get-TargetTriplet)-$Config/.clang-format-files } 2>&1'"
			group = "none"
			options = @{
				env = @{
					LLVM_TOOLS_BINARY_DIR = "`${workspaceFolder}/install/$(Get-HostTriplet)/tools/llvm"
				}
			}
		},
		[ordered] @{
			label = "Clang-Tidy ($Config)"
			type = "shell"
			command = "pwsh -c '& {. `${workspaceFolder}/scripts/env.ps1; Initialize-ToolchainEnv; & python `$env:LLVM_TOOLS_BINARY_DIR/run-clang-tidy -config-file=`${workspaceFolder}/src/.clang-tidy -fix -format -style=file -p `${workspaceFolder}/build/$(Get-TargetTriplet)-$Config/ } 2>&1'"
			group = "none"
			options = @{
				env = @{
					LLVM_TOOLS_BINARY_DIR = "`${workspaceFolder}/install/$(Get-HostTriplet)/tools/llvm"
				}
			}
		}
	)
}

$VSCodeSettings = [ordered] @{
	'clangd.path' = "`${workspaceFolder}/install/$(Get-HostTriplet)/tools/llvm/clangd$($IsWindows ? '.exe' : '')"
	'clangd.arguments' = @(
		'-log=verbose',
		'-pretty',
		'--background-index',
		"--compile-commands-dir=`${workspaceFolder}/build/$(Get-TargetTriplet)-debug"
	)
	'dotnet.defaultSolution' = 'disable'
}

$CMakePresets | ConvertTo-Json -Depth 4 | Out-File "$PSScriptRoot/CMakeUserPresets.json" -Force
$VSCodeLaunchConfiguration | ConvertTo-Json -Depth 5 | Out-File "$PSScriptRoot/.vscode/launch.json" -Force
$VSCodeSettings | ConvertTo-Json -Depth 2 | Out-File "$PSScriptRoot/.vscode/settings.json" -Force
$VSCodeTasks | ConvertTo-Json -Depth 4 | Out-File "$PSScriptRoot/.vscode/tasks.json" -Force

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
if ($LASTEXITCODE -ne 0)
{
	Write-Host "vcpkg install failed with exit code $LASTEXITCODE"
	exit $LASTEXITCODE
}
