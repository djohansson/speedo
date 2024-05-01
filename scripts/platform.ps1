function Get-NativeArchitecture
{
	$Arch = ""
	$ArchInfo = ""

	if ($IsWindows)
	{
		$ArchInfo = $Env:PROCESSOR_ARCHITECTURE
	}
	elseif ($IsMacOS)
	{
		$ArchInfo = machine
	}
	else
	{
		Write-Error "Unsupported Operating System"
	}
	
	if ($ArchInfo -match "amd64")
	{
		$Arch = "x64"
	}
	elseif ($ArchInfo -match "arm64")
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

	if ($IsWindows)
	{
		$OS = "windows"
	}
	elseif ($IsMacOS)
	{
		$OS = "osx"
	}
	else
	{
		Write-Error "Unsupported Operating System" # please implement me
	}

	return $OS
}

function Get-NativeCompiler
{
	$Compiler = ""

	if ($IsWindows -or $IsMacOS)
	{
		$Compiler = "clang"
	}
	else
	{
		Write-Error "Unsupported Operating System" # please implement me
	}

	return $Compiler
}

function Get-NativeTriplet
{
	$Arch = Get-NativeArchitecture
	$OS = Get-NativeOS
	$Compiler = Get-NativeCompiler

	$Triplet = "$Arch-$OS"

	if ($Compiler -ne "")
	{
		$Triplet += "-$Compiler"
	}

	return $Triplet
}
