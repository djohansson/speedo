param([string]$config='release')

. $PSScriptRoot/scripts/env.ps1
. $PSScriptRoot/scripts/platform.ps1

Initialize-SystemEnv

$triplet = Get-NativeTriplet

if ($config -eq 'debug')
{
	Add-EnvPath $PSScriptRoot/build.output/packages/$triplet/debug/bin
	Add-EnvDylibPath $PSScriptRoot/build.output/packages/$triplet/debug/lib
}
else
{
	Add-EnvPath $PSScriptRoot/build.output/packages/$triplet/bin
	Add-EnvDylibPath $PSScriptRoot/build.output/packages/$triplet/lib
}

$env:VK_LAYER_PATH = $PSScriptRoot + '/build.output/packages/' + $triplet + '/share/vulkan/explicit_layer.d'

& $PSScriptRoot/build.output/$config/client -u $HOME/.speedo | Tee-Object -FilePath client.log -Append &
& $PSScriptRoot/build.output/$config/server -u $HOME/.speedo | Tee-Object -FilePath server.log -Append
