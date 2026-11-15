<#
.SYNOPSIS
Returns speedo's version, src/version.h's: the one place it's written.

.DESCRIPTION
src/version.h holds the version (Major, Minor, Patch), which release.ps1 alone changes. Everything else reads it there:
CMakeLists.txt (the project's version, the packages', the crash reports' SPEEDO_VERSION, Info.plist's), the client's
About text (it includes the header), and the scripts through this one (package.ps1, release.ps1, the release workflow).
Returns a [version] of three parts, whose string is <major>.<minor>.<patch>.
#>
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'

$header = [System.IO.File]::ReadAllText((Join-Path $PSScriptRoot '../src/version.h'))
$parts = @('Major', 'Minor', 'Patch') | ForEach-Object {
	if ($header -match "$_ = ([0-9]+)") { [int]$Matches[1] } else { throw "no $_ version in src/version.h" }
}
[version]::new($parts[0], $parts[1], $parts[2])
