<#
.SYNOPSIS
Runs several processes at once on the gpu, and stops them at the first gpu restart (macOS).

.DESCRIPTION
Each command (a sh command line: environment assignments and a program with its arguments) is started at once, its
output to <LogDir>/<Label>-<n>.log. The diagnostic reports are polled meanwhile: at the first new gpuEvent (a gpu restart)
the processes get -Grace seconds (to write their crash reports), then are killed; as they are after -Seconds and 30 more.
The kernel's gpu messages, debug level included (which the persistent log doesn't keep), are streamed to
<LogDir>/<Label>-gpu.log. Prints the outcome, each log's last line (and crash report lines), and any process stuck exiting
(state E: a reboot is due). For rtrepro.mm's runs and the speedo clients beside them (see CLAUDE.md, "Hardware ray
queries"): every run may restart the gpu.

.EXAMPLE
pwsh src/tools/macos/contend.ps1 -Label c1 -Seconds 40 './rtrepro trace 40 rtrepro-rays.txt 1048576 8' './rtrepro burn 40'
#>
[CmdletBinding(PositionalBinding = $false)]
param(
	[Parameter(Mandatory)] [string] $Label,
	[Parameter(Mandatory)] [int] $Seconds,
	[string] $LogDir = (Get-Location).Path,
	[int] $Grace = 3,
	[Parameter(Mandatory, ValueFromRemainingArguments)] [string[]] $Commands
)

$ErrorActionPreference = 'Stop'
$reports = '/Library/Logs/DiagnosticReports'

function Get-GpuEvents
{
	@(Get-ChildItem -Path $reports -Filter 'gpuEvent*' -ErrorAction SilentlyContinue | Sort-Object LastWriteTime)
}

# a command line run by sh (which may cd first), its output to a file; killed with its process tree
function Start-Logged([string] $commandLine, [string] $logPath)
{
	$startInfo = [System.Diagnostics.ProcessStartInfo]::new('/bin/sh')
	$startInfo.ArgumentList.Add('-c')
	$startInfo.ArgumentList.Add("{ $commandLine ; } > '$logPath' 2>&1")
	$startInfo.UseShellExecute = $false
	[System.Diagnostics.Process]::Start($startInfo)
}

$before = (Get-GpuEvents).Count
$stream = Start-Logged "/usr/bin/log stream --level debug --style compact --predicate 'sender CONTAINS ""AGX"" OR sender CONTAINS ""IOGPU""'" (Join-Path $LogDir "$Label-gpu.log")

$processes = @()
for ($commandIt = 0; $commandIt -lt $Commands.Count; $commandIt++)
{
	$processes += Start-Logged $Commands[$commandIt] (Join-Path $LogDir "$Label-$($commandIt + 1).log")
}

$start = Get-Date
$outcome = 'finished'
while ($true)
{
	$events = Get-GpuEvents
	if ($events.Count -gt $before)
	{
		$outcome = "GPU RESTART: $($events[-1].Name)"
		$graceEnd = (Get-Date).AddSeconds($Grace)
		while ((Get-Date) -lt $graceEnd -and ($processes | Where-Object { -not $_.HasExited }))
		{
			Start-Sleep -Milliseconds 500
		}
		break
	}
	if (-not ($processes | Where-Object { -not $_.HasExited }))
	{
		break
	}
	if (((Get-Date) - $start).TotalSeconds -gt $Seconds + 30)
	{
		$outcome = 'time limit'
		break
	}
	Start-Sleep -Milliseconds 500
}
foreach ($process in $processes)
{
	if (-not $process.HasExited)
	{
		$process.Kill($true)
	}
}
Start-Sleep -Seconds 1
$stream.Kill($true)

Write-Output "${Label}: $outcome after $([int]((Get-Date) - $start).TotalSeconds) s"
for ($commandIt = 0; $commandIt -lt $Commands.Count; $commandIt++)
{
	$log = Join-Path $LogDir "$Label-$($commandIt + 1).log"
	$crash = Select-String -Path $log -Pattern 'crash report: ' -SimpleMatch -ErrorAction SilentlyContinue | Select-Object -First 1
	$last = Get-Content $log -Tail 1 -ErrorAction SilentlyContinue
	Write-Output "  $Label-$($commandIt + 1): $(if ($crash) { $crash.Line } else { $last })"
}
$stuck = & ps -o 'pid=,stat=,comm=' -p (($processes | ForEach-Object Id) -join ',') 2>$null | Where-Object { $_ -match '\s\??E' }
if ($stuck)
{
	Write-Output 'STUCK PROCESSES (reboot due):'
	$stuck | Write-Output
}
