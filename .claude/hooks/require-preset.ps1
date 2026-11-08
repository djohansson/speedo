# A PreToolUse hook on Bash (.claude/settings.json): refuses a command that runs `cmake --build <dir>` unless the same
# command configured that build directory with its preset first, joined by && alone: `cmake --preset <name> && cmake
# --build build/<name>` (redirections allowed between them; no ;, |, || or & that would let the build run after a failed
# or skipped configure). A build that reconfigures by itself runs without the preset's environment: vcpkg then computed
# other package hashes, emptied the shared install/ tree and started building LLVM (three times on 2026-11-08). See
# CLAUDE.md, "Always configure via cmake --preset <name>"; CMakeLists.txt also stops such a configure before vcpkg runs.
# Reads the hook's JSON on stdin; denies with a reason, else prints nothing.

$ErrorActionPreference = 'Stop'

$payload = [Console]::In.ReadToEnd() | ConvertFrom-Json
$command = [string]$payload.tool_input.command
if (-not $command)
{
	exit 0
}

function Deny([string] $Reason)
{
	@{
		hookSpecificOutput = @{
			hookEventName = 'PreToolUse'
			permissionDecision = 'deny'
			permissionDecisionReason = $Reason
		}
	} | ConvertTo-Json -Depth 3 -Compress
	exit 0
}

# where it runs as a command (the command's start, or after &&, ||, ;, |, ( or a new line), not text naming it (a
# commit message's, which the hook refused)
foreach ($build in [regex]::Matches($command, '(?<=(?:^|[;&|(\n])\s*)cmake\s+--build\s+("?)(?<dir>[^\s";&|]+)\1'))
{
	$dir = $build.Groups['dir'].Value.TrimEnd('/', '\')
	$name = Split-Path -Leaf $dir
	$before = $command.Substring(0, $build.Index)
	$preset = [regex]::Matches($before, "\bcmake\s+--preset\s+(""?)$([regex]::Escape($name))\1(?=[\s;&|>]|$)") | Select-Object -Last 1
	if (-not $preset)
	{
		Deny "cmake --build $dir needs its own preset configure first, in the same command: cmake --preset $name && cmake --build build/$name (a bare build that reconfigures runs without the preset's environment and makes vcpkg rebuild the shared install/ tree; see CLAUDE.md)"
	}
	# from the configure to the build: && joins them, and nothing lets the build run after a failed configure
	$between = $before.Substring($preset.Index + $preset.Length)
	$chained = $between -match '&&'
	$unsafe = $between -match ';|\n|\|' -or $between -match '(?<![&>])&(?![&>\d])'
	if (-not $chained -or $unsafe)
	{
		Deny "join the preset configure and the build with && alone: cmake --preset $name && cmake --build build/$name (with ;, |, || or & the build runs even when the configure failed, and a failed configure followed by a build emptied install/ before; see CLAUDE.md)"
	}
}
