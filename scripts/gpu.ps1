# what the scripts that run clients share (assettest.ps1, visualtest.ps1, researchcompare.ps1, dot-sourced): the gpu's
# lock, and stopping a client that ran past its time.
#
# the gpu's lock, on macOS alone: Windows and Linux run their clients side by side whatever they trace, and take none
# (the user's call, 2026-11-28). on macOS a client tracing with hardware ray queries (SPEEDO_RAY_QUERY=1: Metal's ray
# tracing, or KosmicKrisp's) hangs or restarts the gpu when other gpu work runs beside it (Apple's bugs, see
# docs/notes/indirect-lighting.md, "Hardware ray queries"): visualtest's ray query checks run beside the quick tier hung
# it, and a tier client was left stuck in the kernel (2026-11-27). so a run whose clients take ray queries there takes
# the lock alone, and every other run shares it: either waits for the other, whoever started it. a file lock
# (temp/gpu.lock: flock on Unix, a share mode on Windows, through .NET's FileShare), which the system lets go when the
# process holding it ends, however it ends; who holds it is noted beside it (temp/gpu.lock.d/<pid>), for a run that
# waits to say what it waits for.

# whether a run takes the lock alone: on macOS where its clients take ray queries (the variable set, or set by the run
# for its clients)
function Test-GpuAlone([string[]] $RayQueryValues = @())
{
	if (-not $IsMacOS) { return $false }
	return $env:SPEEDO_RAY_QUERY -eq '1' -or $RayQueryValues -contains '1'
}

# takes the lock (alone, or shared), waiting as long as it takes and saying what it waits for (every 5 minutes again);
# held until the script's process ends. name: the run's, for the note. nothing off macOS
function Enter-GpuLock([string] $Name, [bool] $Alone)
{
	if (-not $IsMacOS) { return }
	$directory = Join-Path (Split-Path -Parent $PSScriptRoot) 'temp'
	$notes = Join-Path $directory 'gpu.lock.d'
	New-Item -ItemType Directory -Force -Path $notes | Out-Null
	$path = Join-Path $directory 'gpu.lock'
	$share = if ($Alone) { [IO.FileShare]::None } else { [IO.FileShare]::ReadWrite }
	# (a run waiting to take it alone says so, and a shared one waits behind it: else shared runs, one starting as another
	# ends, could keep it from ever getting it)
	$waiting = Join-Path $notes "waiting-$PID"
	if ($Alone) { Set-Content -LiteralPath $waiting -Value $Name }
	$said = [datetime]::MinValue
	while ($true)
	{
		try
		{
			if (-not $Alone -and (Test-GpuLockWaiter $notes)) { throw [IO.IOException]::new('a run waits to take it alone') }
			$script:gpuLock = [IO.FileStream]::new($path, [IO.FileMode]::OpenOrCreate, [IO.FileAccess]::ReadWrite, $share)
			break
		}
		catch [IO.IOException]
		{
			if (((Get-Date) - $said).TotalMinutes -ge 5)
			{
				$holders = @(Get-GpuLockHolders $notes)
				$held = if ($holders.Count -gt 0) { $holders -join '; ' } else { 'a run that left no note' }
				[Console]::Error.WriteLine("waiting for the gpu ($(if ($Alone) { 'alone, ray queries on macOS' } else { 'shared' })): held by $held")
				$said = Get-Date
			}
			Start-Sleep -Seconds 2
		}
	}
	Remove-Item -LiteralPath $waiting -ErrorAction SilentlyContinue
	$note = "$Name ($(if ($Alone) { 'alone' } else { 'shared' }), pid $PID, since $((Get-Date).ToString('HH:mm:ss')))"
	Set-Content -LiteralPath (Join-Path $notes "$PID") -Value $note
}

# whether a run of a process still there waits to take the lock alone (its note waiting-<pid>; those of processes gone
# removed)
function Test-GpuLockWaiter([string] $Notes)
{
	$any = $false
	foreach ($file in Get-ChildItem -LiteralPath $Notes -File -Filter 'waiting-*' -ErrorAction SilentlyContinue)
	{
		$id = $file.Name.Substring('waiting-'.Length) -as [int]
		if ($id -and (Get-Process -Id $id -ErrorAction SilentlyContinue)) { $any = $true }
		else { Remove-Item -LiteralPath $file.FullName -ErrorAction SilentlyContinue }
	}
	return $any
}

# the notes of the processes holding the lock, those of processes gone removed
function Get-GpuLockHolders([string] $Notes)
{
	foreach ($file in Get-ChildItem -LiteralPath $Notes -File -ErrorAction SilentlyContinue)
	{
		if ($file.Name.StartsWith('waiting-')) { continue }
		$id = $file.Name -as [int]
		if ($id -and (Get-Process -Id $id -ErrorAction SilentlyContinue))
		{
			Get-Content -LiteralPath $file.FullName -ErrorAction SilentlyContinue | Select-Object -First 1
		}
		else
		{
			Remove-Item -LiteralPath $file.FullName -ErrorAction SilentlyContinue
		}
	}
}

# stops a client past its time: killed with what it started, then waited for, a while. false where it hasn't ended by
# then: on macOS a client stuck in the kernel after the gpu hung (state E in ps) outlives a SIGKILL, and a wait without
# an end on one held the quick tier for 10 hours (2026-11-27). the gpu is wedged then, and only a reboot frees it: the
# caller stops its run (see Write-GpuWedged)
#   (SPEEDO_CLIENT_KILL_WAIT_SECONDS: how long, 30 by default. a test's: at 0 the wait ends before the kill is through,
#   as a client that doesn't end does, to see a run stop)
function Stop-Client([Diagnostics.Process] $Process, [int] $Seconds = $(if ($env:SPEEDO_CLIENT_KILL_WAIT_SECONDS) { [int]$env:SPEEDO_CLIENT_KILL_WAIT_SECONDS } else { 30 }))
{
	try { $Process.Kill($true) } catch [InvalidOperationException] { }
	return $Process.WaitForExit($Seconds * 1000)
}

function Write-GpuWedged([Diagnostics.Process] $Process, [string] $Log)
{
	$state = if (-not $IsWindows) { (& ps -o stat= -p $Process.Id 2>$null) -join '' } else { '' }
	[Console]::Error.WriteLine("STOP: a client (pid $($Process.Id)$(if ($state) { ", state $($state.Trim())" })) didn't end when killed: the gpu is likely wedged, and only a reboot frees it. the run stops here (log: $Log)")
}
