param([string[]]$Programs, [string]$Config='release')

. $PSScriptRoot/scripts/env.ps1

Initialize-SystemEnv
Initialize-VcpkgEnv

$env:VK_LAYER_PATH = $PSScriptRoot + '/build/packages/' + $env:TARGET_TRIPLET + '/share/vulkan/explicit_layer.d'
if ($IsMacOS)
{
	$env:VK_DRIVER_FILES = $(brew --prefix molten-vk) + '/share/vulkan/icd.d/MoltenVK_icd.json'
}

foreach ($program in $Programs)
{
	$programPath = $PSScriptRoot + '/build/' + $env:TARGET_TRIPLET + '/' + $config + '/' + $program
	if (-not (Test-Path $programPath))
	{
		Write-Host "Program $program not found at $programPath"
		exit 1
	}

	$processOptions = @{
		FilePath = $programPath
		RedirectStandardOutput = $program + ".stdout.log"
		RedirectStandardError = $program + ".stderr.log"
		ArgumentList = "-u $HOME/.speedo"
		NoNewWindow = $true
	}
	Start-Process @processOptions
}

exit 0
