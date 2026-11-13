param([Parameter(Mandatory = $true)][string]$Binary, [string]$Backend = 'NONE')
$ErrorActionPreference = 'Stop'

function Start-ShatterEngine([int]$Cycle) {
    $startInfo = [System.Diagnostics.ProcessStartInfo]::new()
    $startInfo.FileName = $Binary
    $startInfo.Arguments = 'start'
    $startInfo.UseShellExecute = $false
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true
    $process = [System.Diagnostics.Process]::new()
    $process.StartInfo = $startInfo
    try {
        Write-Host "Starting engine (cycle $Cycle)"
        if (-not $process.Start()) { throw 'could not launch shatter start' }
        $stdout = $process.StandardOutput.ReadToEndAsync()
        $stderr = $process.StandardError.ReadToEndAsync()
        if (-not $process.WaitForExit(10000)) {
            $process.Kill()
            throw "shatter start (cycle $Cycle) did not exit within 10 seconds"
        }
        # The engine stays alive, but it must not retain the startup CLI's pipes.
        $readers = [System.Threading.Tasks.Task[]]@($stdout, $stderr)
        if (-not [System.Threading.Tasks.Task]::WaitAll($readers, 2000)) {
            throw "shatter start (cycle $Cycle) exited but its output pipes remained open in the detached engine"
        }
        if ($process.ExitCode -ne 0) {
            throw "shatter start (cycle $Cycle) exited with $($process.ExitCode): $($stderr.Result)"
        }
        if ($stdout.Result -notmatch 'engine started') {
            throw "unexpected startup response in cycle ${Cycle}: $($stdout.Result)"
        }
    }
    finally {
        $process.Dispose()
    }
}

try {
    # Exercise fresh backend construction and COM teardown more than once in
    # the same test run. A single start/stop only verifies process startup.
    for ($cycle = 1; $cycle -le 2; $cycle++) {
        Start-ShatterEngine $cycle

        Write-Host "Checking engine commands (cycle $cycle)"
        $status = (& $Binary status | Out-String)
        $expectedStatus = if ($Backend -eq 'WASAPI') { 'is running;' } else { 'is running; audio-unavailable:.*no audio backend is configured' }
        if ($LASTEXITCODE -ne 0 -or $status -notmatch $expectedStatus) {
            throw "unexpected running status in cycle ${cycle}: $status"
        }

        $inputs = (& $Binary list in | Out-String)
        $expectedInputs = if ($Backend -eq 'WASAPI') { 'Windows WASAPI playback sessions' } else { 'no audio backend is configured' }
        if ($LASTEXITCODE -ne 0 -or $inputs -notmatch $expectedInputs) {
            throw "unexpected input listing in cycle ${cycle}: $inputs"
        }

        if ($Backend -eq 'WASAPI') {
            $outputs = (& $Binary list out | Out-String)
            if ($LASTEXITCODE -ne 0 -or $outputs -notmatch 'Windows WASAPI playback endpoints') {
                throw "unexpected endpoint listing in cycle ${cycle}: $outputs"
            }
            $health = (& $Binary health | Out-String)
            if ($LASTEXITCODE -ne 0 -or $health -notmatch 'Engine health: (running|degraded)') {
                throw "unexpected WASAPI health report in cycle ${cycle}: $health"
            }

            $unsupported = (& $Binary assign app 'Shatter unsupported test app' 'Shatter unsupported test endpoint' 2>&1 | Out-String)
            $unsupportedExitCode = $LASTEXITCODE
            if ($unsupportedExitCode -eq 0 -or $unsupported -notmatch 'WASAPI currently supports playback session and endpoint discovery only') {
                throw "WASAPI did not clearly reject app routing in cycle ${cycle}: $unsupported"
            }
        }

        Write-Host "Stopping engine (cycle $cycle)"
        & $Binary stop | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "shatter stop (cycle $cycle) exited with $LASTEXITCODE" }

        $status = (& $Binary status | Out-String)
        if ($LASTEXITCODE -ne 0 -or $status -notmatch 'is stopped') {
            throw "unexpected stopped status in cycle ${cycle}: $status"
        }
    }
}
finally {
    & $Binary stop | Out-Null
}
