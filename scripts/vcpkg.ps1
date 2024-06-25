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
	param([Parameter(Mandatory = $True, ValueFromPipeline=$True, Position = 0)] [string] $triplet)

	Write-Host "Setting up vcpkg environment..."

	if (Test-Path -Path "~/.vcpkg/vcpkg-init.ps1")
	{
		. ~/.vcpkg/vcpkg-init.ps1
	}
	elseif (-not (Test-Path -Path "~/.vcpkg/vcpkg"))
	{
		Write-Host "Installing vcpkg..."

		if ($IsLinux)
		{
			git clone https://github.com/microsoft/vcpkg.git ~/.vcpkg

			~/.vcpkg/bootstrap-vcpkg.sh
		}
		else
		{
			Invoke-Expression(Invoke-WebRequest -useb https://aka.ms/vcpkg-init.ps1)
		}
	}

	Write-Host "Installing vcpkg packages for $triplet using manifest..."

	~/.vcpkg/vcpkg install --x-install-root=$PSScriptRoot/../build.vcpkg --overlay-triplets=$PSScriptRoot/triplets --triplet $triplet --x-feature=client --x-feature=server --no-print-usage
}
