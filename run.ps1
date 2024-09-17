param([string]$config='release')

. $PSScriptRoot/scripts/env.ps1
. $PSScriptRoot/scripts/platform.ps1

Initialize-SystemEnv

$triplet = Get-NativeTriplet

if ($config -eq 'debug')
{
	Add-EnvPath $PSScriptRoot/build/packages/$triplet/debug/bin
	Add-EnvDylibPath $PSScriptRoot/build/packages/$triplet/debug/lib
}
else
{
	Add-EnvPath $PSScriptRoot/build/packages/$triplet/bin
	Add-EnvDylibPath $PSScriptRoot/build/packages/$triplet/lib
}

$env:VK_LAYER_PATH = $PSScriptRoot + '/build/packages/' + $triplet + '/share/vulkan/explicit_layer.d'

& $PSScriptRoot/build/$config/client -u $HOME/.speedo | Tee-Object -FilePath client.log -Append &
& $PSScriptRoot/build/$config/server -u $HOME/.speedo | Tee-Object -FilePath server.log -Append
