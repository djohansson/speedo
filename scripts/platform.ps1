function Get-NativeArchitecture
{
	$Arch = ""
	$ArchInfo = ""

	if (!(Test-Path Variable:\IsWindows) -or $IsWindows) 
	{
		$ArchInfo = $Env:PROCESSOR_ARCHITECTURE
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

function Get-NativeOS
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

function Get-TargetTriplet
{
	$Arch = Get-NativeArchitecture
	$OS = Get-NativeOS
	$Compiler = "clang"

	return "$Arch-$OS-$Compiler"
}
