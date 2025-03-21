. $PSScriptRoot/platform.ps1

# New-Module -name vcpkg -ScriptBlock
# {
# 	function vcpkg()
# 	{
# 		$env:Z_VCPKG_POSTSCRIPT = Join-Path ([System.IO.Path]::GetTempPath()) "VCPKG_tmp_$(Get-Random -SetSeed $PID).ps1"
# 		& ~/.vcpkg/vcpkg @args
# 		if (Test-Path $env:Z_VCPKG_POSTSCRIPT)
# 		{
# 			$postscr = Get-Content -Raw $env:Z_VCPKG_POSTSCRIPT
# 			if ($postscr)
# 			{
# 				iex $postscr
# 			}
# 			Remove-Item -Force -ea 0 $env:Z_VCPKG_POSTSCRIPT,env:Z_VCPKG_POSTSCRIPT
# 		}
# 	}
# } | Out-Null

function Initialize-Vcpkg
{
	Write-Host "Setting up vcpkg environment..."

	$VcpkgRoot = "~/.vcpkg"

	if (Test-Path -Path "$VcpkgRoot/vcpkg-init.ps1")
	{
		. $VcpkgRoot/vcpkg-init.ps1
	}
	elseif (-not (Test-Path -Path "$VcpkgRoot/vcpkg"))
	{
		Write-Host "Installing vcpkg..."

		if ($IsLinux)
		{
			git clone https://github.com/microsoft/vcpkg.git $VcpkgRoot

			Invoke-Expression("$VcpkgRoot/bootstrap-vcpkg.sh")
		}
		else
		{
			Invoke-Expression(Invoke-WebRequest -useb https://aka.ms/vcpkg-init.ps1)
		}
	}

	return $VcpkgRoot
}
