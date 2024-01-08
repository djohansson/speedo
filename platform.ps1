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
		Write-Error "Unsupported architecture: $ArchInfo"
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
		Write-Error "Unsupported Operating System"
	}

	return $OS
}

function Get-NativeDefaultCompiler
{
	$Compiler = ""

	if ($IsWindows -or $IsMacOS)
	{
		$Compiler = "clang"
	}
	else
	{
		Write-Error "Unsupported Operating System"
	}

	return $Compiler
}

function Get-NativeTriplet
{
	$Arch = Get-NativeArchitecture
	$OS = Get-NativeOS
	$Compiler = Get-NativeDefaultCompiler

	$Triplet = "$Arch-$OS"

	if ($Compiler -ne "")
	{
		$Triplet += "-$Compiler"
	}

	return $Triplet
}
