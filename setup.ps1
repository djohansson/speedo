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

# values that, when changed by an OS/SDK/toolchain update, invalidate existing CMake build directories; VCPKG_ROOT too, as
# the build directories cache the toolchain file under it (a vcpkg release bump, or the submodule's retirement)
$PlatformStateKeys = @('SDKROOT', 'MACOS_BUILD_VERSION', 'WINDOWS_SDK_VERSION', 'VISUAL_STUDIO_VCTOOLS_VERSION', 'VCPKG_ROOT')
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

	# vcpkg without a clone (its standalone deployment): the tool and its scripts bundle (scripts and triplets, no ports)
	# of the release scripts/vcpkg-tool.json pins, each release in a directory of its own under ~/.vcpkg-tool
	# (SPEEDO_VCPKG_ROOT for another; not ~/.vcpkg, vcpkg's own one-line install, which updates itself), checked by
	# their SHA-512; the ports come from the git registry vcpkg-configuration.json names. Until 2026-11-09 vcpkg was a
	# submodule, a clone of its repository
	$VcpkgTool = Get-Content -Raw -Path "$PSScriptRoot/scripts/vcpkg-tool.json" | ConvertFrom-Json
	$VcpkgRoot = $env:SPEEDO_VCPKG_ROOT ? $env:SPEEDO_VCPKG_ROOT : (Join-Path $HOME '.vcpkg-tool' $VcpkgTool.release)
	if (!(Test-Path (Join-Path $VcpkgRoot "vcpkg$exeSuffix")))
	{
		$ToolKey = $Platform -eq 'osx' ? 'osx' : "$Platform-$(Get-HostArchitecture)"
		$Tool = $VcpkgTool.tool.$ToolKey
		if (!$Tool) { throw "scripts/vcpkg-tool.json names no vcpkg tool for $ToolKey" }
		$ReleaseUrl = "https://github.com/microsoft/vcpkg-tool/releases/download/$($VcpkgTool.release)"
		$Partial = "$VcpkgRoot.partial"
		Remove-Item -Recurse -Force -Path $Partial -ErrorAction SilentlyContinue
		New-Item -ItemType Directory -Force -Path $Partial | Out-Null
		foreach ($Download in @(@{ File = $VcpkgTool.bundle.file; Sha512 = $VcpkgTool.bundle.sha512 }, @{ File = $Tool.file; Sha512 = $Tool.sha512 }))
		{
			Write-Host "Downloading vcpkg $($VcpkgTool.release): $($Download.File)..."
			$Path = Join-Path $Partial $Download.File
			Invoke-WebRequest -Uri "$ReleaseUrl/$($Download.File)" -OutFile $Path
			if ((Get-FileHash -Algorithm SHA512 -Path $Path).Hash -ne $Download.Sha512) { throw "vcpkg's $($Download.File) doesn't match its SHA-512 in scripts/vcpkg-tool.json" }
		}
		tar -xzf (Join-Path $Partial $VcpkgTool.bundle.file) -C $Partial
		if ($LASTEXITCODE -ne 0) { throw "extracting vcpkg's scripts bundle failed with exit code $LASTEXITCODE" }
		Remove-Item -Force -Path (Join-Path $Partial $VcpkgTool.bundle.file)
		Move-Item -Force -Path (Join-Path $Partial $Tool.file) -Destination (Join-Path $Partial "vcpkg$exeSuffix")
		if (!$IsWindows) { chmod +x (Join-Path $Partial 'vcpkg') }
		Move-Item -Path $Partial -Destination $VcpkgRoot
	}
}
else
{
	Write-Error "Unsupported Operating System" # please implement me
}

$global:Env['VCPKG_ROOT'] = $VcpkgRoot
# the sources ports download, shared by every clone and vcpkg release (VCPKG_DOWNLOADS to put them elsewhere); the ports'
# build trees and packages are the clone's own, under temp/vcpkg (see $VcpkgOptions and the presets)
$global:Env['VCPKG_DOWNLOADS'] = $env:VCPKG_DOWNLOADS ? $env:VCPKG_DOWNLOADS : (Join-Path $HOME '.vcpkg-tool' 'downloads')
New-Item -ItemType Directory -Force -Path $global:Env['VCPKG_DOWNLOADS'] | Out-Null

# the llvm port's version, which the clang triplets track in their packages' ABI (VCPKG_ENV_PASSTHROUGH): the compiler is
# that port's (a host package, which vcpkg's compiler tracking can't see, VCPKG_DISABLE_COMPILER_TRACKING), so without it
# a shared binary cache would hand out packages built by an older clang against an older libc++ after an LLVM update
$LlvmPort = Get-Content -Path "$PSScriptRoot/ports/llvm/vcpkg.json" -Raw | ConvertFrom-Json
$global:Env['LLVM_PORT_VERSION'] = "$($LlvmPort.version)#$($LlvmPort.'port-version' ?? 0)"

# vcpkg's binary sources: the local cache and the shared one (see scripts/binarycache.ps1); the AWS CLI's endpoint and
# region where this machine writes to the shared one (SPEEDO_BINARY_CACHE_WRITE)
. $PSScriptRoot/scripts/binarycache.ps1
$global:Env['VCPKG_BINARY_SOURCES'] = Get-BinaryCacheSources
foreach ($Entry in (Get-BinaryCacheAwsEnvironment).GetEnumerator())
{
	if ($env:SPEEDO_BINARY_CACHE_WRITE) { $global:Env[$Entry.Key] = $Entry.Value } else { $global:Env.Remove($Entry.Key) }
}

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
				VCPKG_INSTALL_OPTIONS = '--x-abi-tools-use-exact-versions;--no-print-usage;--x-buildtrees-root=${sourceDir}/temp/vcpkg/buildtrees;--x-packages-root=${sourceDir}/temp/vcpkg/packages' #;--debug'
				VCPKG_INSTALLED_DIR = "`${sourceDir}/install" # reminder: installDir (cmake), VCPKG_INSTALLED_DIR (vcpkg, same as --x-install-root)
				VCPKG_OVERLAY_TRIPLETS = '${sourceDir}/scripts/cmake/triplets'
				VCPKG_MANIFEST_DIR = '${sourceDir}'
				# needs to be duplicated here to be work in the vscode cmake extension
				VCPKG_DISABLE_COMPILER_TRACKING = 'ON' # This target is not compiled yet when vcpkg wants to calculate the compiler hash.
				CMAKE_EXPORT_COMPILE_COMMANDS = 'ON'
				CMAKE_MAP_IMPORTED_CONFIG_PROFILE = 'profile;release'
				CMAKE_FASTBUILD_USE_DETERMINISTIC_PATHS = 'ON'
				CMAKE_FASTBUILD_USE_LIGHTCACHE='ON'
				# relative paths in the compile commands, so that FASTBuild's cache keys are the same wherever the checkout
				# is (CI's runners, other machines; see CMakeLists.txt for the libc++ headers)
				CMAKE_FASTBUILD_USE_RELATIVE_PATHS = 'ON'
				#
			}
			environment = [ordered] @{
				VCPKG_ROOT = "$env:VCPKG_ROOT"
				VCPKG_DOWNLOADS = "$env:VCPKG_DOWNLOADS"
				# the binary cache's sources and the llvm port's version (tracked by the clang triplets), as setup's own vcpkg
				# run has them, for the vcpkg runs a configure starts (the vscode cmake extension's too)
				VCPKG_BINARY_SOURCES = "$env:VCPKG_BINARY_SOURCES"
				LLVM_PORT_VERSION = "$env:LLVM_PORT_VERSION"
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
				# the project's own code for the fixed baseline the vcpkg ports are built for (see clang.toolchain.cmake), the cpu
				# of CI's runners, never the building machine's: what a machine builds is shared (FASTBuild's cache, packages),
				# and -march=native would hand an M1 an M4's instructions (the user's call, 2026-11-11). a build directory
				# configured for native before keeps its flags (CMakeLists.txt refuses it: configure it with --fresh)
				SPEEDO_TARGET_CPU = 'baseline'
				#
				# FBuild run through fastbuild-s3cache's launcher, which turns FASTBuild's cache on with its plugin: a local
				# directory, then the shared bucket's speedo/fastbuild/ (see ports/fastbuild-s3cache, docs/notes/build.md,
				# "FASTBuild's cache"). the launcher reads the remote's url and region from the build directory's cache
				CMAKE_MAKE_PROGRAM = "`${sourceDir}/install/$(Get-HostTriplet)/tools/fastbuild-s3cache/fbuild-s3cache$(if ($IsWindows) { '.exe' } else { '' })"
				SPEEDO_FASTBUILD_CACHE_URL = "$(Get-FastbuildCacheUrl)"
				SPEEDO_FASTBUILD_CACHE_REGION = "$(Get-FastbuildCacheRegion)"
			}
			environment = [ordered] @{
				LLVM_ROOT = "`${sourceDir}/install/$(Get-HostTriplet)"
				LLVM_TOOLS_BINARY_DIR = "`${sourceDir}/install/$(Get-HostTriplet)/tools/llvm"
			}
		}
	)
	buildPresets = @()
}
# where this machine writes to the shared binary cache, the AWS CLI's endpoint and region for the vcpkg runs a configure starts
if ($env:SPEEDO_BINARY_CACHE_WRITE)
{
	foreach ($Entry in (Get-BinaryCacheAwsEnvironment).GetEnumerator()) { $CMakePresets.configurePresets[0].environment[$Entry.Key] = $Entry.Value }
}
# Tracy's profiler app (vcpkg.json's profiler-app feature, a default one: tracy[gui-tools] and the 13 packages it alone
# needs) for local development; SPEEDO_PROFILER_APP=0 leaves it out of setup's install and the configures' (the manifest's
# other default features alone), as CI and the release workflow do: they never run it
$ProfilerApp = $env:SPEEDO_PROFILER_APP -ne '0'
$ManifestFeatures = @('client', 'server')
if (-not $ProfilerApp)
{
	$CMakePresets.configurePresets[0].cacheVariables['VCPKG_MANIFEST_NO_DEFAULT_FEATURES'] = 'ON'
	$CMakePresets.configurePresets[0].cacheVariables['VCPKG_MANIFEST_FEATURES'] = $ManifestFeatures -join ';'
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
			MACOSX_DEPLOYMENT_TARGET = "$env:MACOSX_DEPLOYMENT_TARGET" # tracked by the osx triplets (the SDKROOT path is not)
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
			# the mesa port's KosmicKrisp (in the host triplet's install tree), for the launch configurations
			[ordered] @{
				label = "export-kosmickrisp-driver-path"
				type = "shell"
				command = "pwsh -c '& { if (-not (Test-Path `${userHome}/.speedo)) { New-Item -Force -ItemType Directory -Path `${userHome}/.speedo | Out-Null }; `"VK_DRIVER_FILES=`$((Get-ChildItem `${workspaceFolder}/install/*/share/vulkan/icd.d/kosmickrisp_mesa_icd.*.json | Select-Object -First 1).FullName)`" > `${userHome}/.speedo/.env }' 2>&1"
				group = "none"
			}
		)
	}
	@{ # linux
		HostSystemName = 'Linux'
		PresetCacheVariables = [ordered] @{}
		PresetEnvironment = [ordered] @{
			LD_LIBRARY_PATH = "`$penv{LD_LIBRARY_PATH}:`$env{LLVM_ROOT}/lib"
			GLIBC_VERSION = "$env:GLIBC_VERSION" # tracked by the linux clang triplets
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
	if ($ProfilerApp)
	{
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
	}

	$VSCodeTasks.tasks += $Settings.Tasks

	# the profile configuration with our own targets instrumented, without mimalloc (see SPEEDO_SANITIZE in
	# CMakeLists.txt): sanitize by asan and ubsan, tsan by tsan (which combines with neither). the runtimes are
	# compiler-rt's, which the host llvm builds only off windows
	if (-not $IsWindows)
	{
		foreach ($Sanitizer in ([ordered] @{ sanitize = 'address,undefined'; tsan = 'thread' }).GetEnumerator())
		{
			$CMakePresets.configurePresets += @(
				[ordered] @{
					name = "$(Get-TargetTriplet)-$($Sanitizer.Key)"
					inherits = "$(Get-TargetTriplet)-profile"
					cacheVariables = [ordered] @{
						SPEEDO_SANITIZE = $Sanitizer.Value
					}
				}
			)
			$CMakePresets.buildPresets += @(
				[ordered] @{
					name = "$(Get-TargetTriplet)-$($Sanitizer.Key)"
					configurePreset = "$(Get-TargetTriplet)-$($Sanitizer.Key)"
					configuration = 'profile'
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
			) + @(if ($ProfilerApp) { "($(Get-TargetTriplet)) tracy-profiler" })
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
	# build outputs, vcpkg packages and the downloaded test assets (several GB) keep the file watcher busy
	'files.watcherExclude' = [ordered] @{
		'**/build/**' = $true
		'**/install/**' = $true
		'**/temp/**' = $true
		'**/resources/test-assets/**' = $true
	}
}

$CMakePresets | ConvertTo-Json -Depth 4 | Out-File "$PSScriptRoot/CMakeUserPresets.json" -Force
# a fresh clone has no .vscode (it isn't tracked), and Out-File doesn't create directories
New-Item -ItemType Directory -Force -Path "$PSScriptRoot/.vscode" | Out-Null
$VSCodeLaunchConfiguration | ConvertTo-Json -Depth 5 | Out-File "$PSScriptRoot/.vscode/launch.json" -Force
$VSCodeSettings | ConvertTo-Json -Depth 2 | Out-File "$PSScriptRoot/.vscode/settings.json" -Force
$VSCodeTasks | ConvertTo-Json -Depth 4 | Out-File "$PSScriptRoot/.vscode/tasks.json" -Force

$env:LLVM_ROOT = "$PSScriptRoot/install/$(Get-HostTriplet)"
$env:LLVM_TOOLS_BINARY_DIR = "$PSScriptRoot/install/$(Get-HostTriplet)/tools/llvm"

# an argument list (not a command line for Invoke-Expression): VCPKG_ROOT is under the home directory, which may hold spaces
$VcpkgOptions = @(
	"--vcpkg-root=$env:VCPKG_ROOT"
	"--x-install-root=$PSScriptRoot/install"
	"--x-buildtrees-root=$PSScriptRoot/temp/vcpkg/buildtrees"
	"--x-packages-root=$PSScriptRoot/temp/vcpkg/packages"
	"--overlay-triplets=$PSScriptRoot/scripts/cmake/triplets"
	"--host-triplet=$(Get-HostTriplet)"
	"--triplet=$(Get-TargetTriplet)"
	'--x-abi-tools-use-exact-versions'
	'--no-print-usage'
) + @(if (-not $ProfilerApp) { @('--x-no-default-features') + @($ManifestFeatures | ForEach-Object { "--x-feature=$_" }) })

$Vcpkg = Join-Path $env:VCPKG_ROOT "vcpkg$exeSuffix"
# the binary cache first, building nothing (--only-binarycaching): where it holds every package, the tools only building
# ports from source takes (the system's compilers for the host triplets, meson's and autotools' tools: the platform
# script's -PortBuildTools) aren't installed. Where it lacks one, they are, and the install goes on from what it restored.
# The restore stops at the first package the cache lacks with vcpkg's report of a failed build (red, advising to update
# the port files and to report an issue), which here only means that package is to be built: so vcpkg's output goes
# through a pipe, where it prints no colors, and the report gives way to a line naming the package
Write-Host "Restoring vcpkg's packages from the binary cache..."
$CacheMissing = $null
$InFailureReport = $false
& $Vcpkg install @VcpkgOptions --only-binarycaching 2>&1 | ForEach-Object {
	$Line = "$_"
	if ($Line -match '^Installing \d+/\d+ (\S+)\.\.\.$') { $CacheMissing = $Matches[1] }
	elseif ($Line.StartsWith("Please ensure you're using the latest port files")) { $InFailureReport = $true }
	if (-not $InFailureReport) { Write-Host $Line }
}
if ($LASTEXITCODE -ne 0)
{
	Write-Host "Not every package is in the binary cache$($CacheMissing ? " ($CacheMissing is the first that isn't)" : ''):" `
		"installing the tools that build ports, then building them"
	& $PSScriptRoot/scripts/platforms/$Platform/$Platform.ps1 -PortBuildTools
	& $Vcpkg install @VcpkgOptions
}
if ($LASTEXITCODE -ne 0)
{
	Write-Host "vcpkg install failed with exit code $LASTEXITCODE"
	exit $LASTEXITCODE
}
