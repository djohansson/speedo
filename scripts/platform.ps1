function Get-HostArchitecture
{
	$Arch = ""
	$ArchInfo = ""

	if (!(Test-Path Variable:\IsWindows) -or $IsWindows) 
	{
		$ArchInfo = [Environment]::GetEnvironmentVariable("PROCESSOR_ARCHITECTURE", "Machine")
		$Env:PROCESSOR_ARCHITECTURE = $ArchInfo
	}
	elseif ($IsMacOS)
	{
		$ArchInfo = Invoke-Expression "$(brew --prefix)/opt/coreutils/libexec/gnubin/uname -m"
		$Env:PROCESSOR_ARCHITECTURE = $ArchInfo
	}
	else
	{
		$ArchInfo = uname -m
		$Env:PROCESSOR_ARCHITECTURE = $ArchInfo
	}
	
	if ($ArchInfo -in "amd64", "AMD64", "x86_64")
	{
		$Arch = "x64"
	}
	elseif ($ArchInfo -in "arm64", "aarch64")
	{
		$Arch = "arm64"
	}
	else
	{
		Write-Error "Unsupported architecture: $ArchInfo" # please implement me
	}

	return $Arch
}

function Get-HostOS
{
	$OS = ""

	if (!(Test-Path Variable:\IsWindows) -or $IsWindows) 
	{
		$OS = "windows"
	}
	elseif ($IsMacOS)
	{
		$OS = "osx"
	}
	elseif ($IsLinux)
	{
		$OS = "linux"
	}
	else
	{
		Write-Error "Unsupported Operating System" # please implement me
	}

	return $OS
}

function Get-HostCompiler
{
	if (!(Test-Path Variable:\IsWindows) -or $IsWindows) 
	{
		$Compiler = "clangcl"
	}
	elseif ($IsMacOS)
	{
		$Compiler = "release" # use default native compiler
	}
	elseif ($IsLinux)
	{
		$Compiler = "release" # use default native compiler
	}
	else
	{
		Write-Error "Unsupported Operating System" # please implement me
	}

	return $Compiler
}

function Get-HostTuplet
{
	return "$(Get-HostArchitecture)-$(Get-HostOS)"
}

function Get-HostTriplet
{
	return "$(Get-HostTuplet)-$(Get-HostCompiler)"
}

function Get-TargetTuplet
{
	return "$(Get-HostTuplet)"
}

function Get-TargetTriplet
{
	return "$(Get-TargetTuplet)-clang"
}