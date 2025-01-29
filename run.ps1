param([string]$config='RelWithDebInfo')

. $PSScriptRoot/scripts/env.ps1
. $PSScriptRoot/scripts/platform.ps1

Initialize-SystemEnv

$triplet = Get-TargetTriplet

if ($config -eq 'Debug')
{
	Add-EnvDylibPath $PSScriptRoot/build/packages/$triplet/debug
}
else
{
	Add-EnvDylibPath $PSScriptRoot/build/packages/$triplet
}

$env:VK_LAYER_PATH = $PSScriptRoot + '/build/packages/' + $triplet + '/share/vulkan/explicit_layer.d'

& $PSScriptRoot/build/$triplet/$config/client -u $HOME/.speedo | Tee-Object -FilePath client.log -Append &
& $PSScriptRoot/build/$triplet/$config/server -u $HOME/.speedo | Tee-Object -FilePath server.log -Append
