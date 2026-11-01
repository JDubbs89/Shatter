param([Parameter(Mandatory = $true)][string]$Binary, [string]$Backend = 'NONE')
$ErrorActionPreference = 'Stop'

try {
    & $Binary start | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "shatter start exited with $LASTEXITCODE" }

    $status = (& $Binary status | Out-String)
    $expectedStatus = if ($Backend -eq 'WASAPI') { 'is running;' } else { 'is running; audio-unavailable:.*no audio backend is configured' }
    if ($LASTEXITCODE -ne 0 -or $status -notmatch $expectedStatus) {
        throw "unexpected running status: $status"
    }

    $inputs = (& $Binary list in | Out-String)
    $expectedInputs = if ($Backend -eq 'WASAPI') { 'Windows WASAPI playback sessions' } else { 'no audio backend is configured' }
    if ($LASTEXITCODE -ne 0 -or $inputs -notmatch $expectedInputs) {
        throw "unexpected input listing: $inputs"
    }

    if ($Backend -eq 'WASAPI') {
        $outputs = (& $Binary list out | Out-String)
        if ($LASTEXITCODE -ne 0 -or $outputs -notmatch 'Windows WASAPI playback endpoints') {
            throw "unexpected endpoint listing: $outputs"
        }
        $health = (& $Binary health | Out-String)
        if ($LASTEXITCODE -ne 0 -or $health -notmatch 'Engine health: (running|degraded)') {
            throw "unexpected WASAPI health report: $health"
        }
    }

    & $Binary stop | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "shatter stop exited with $LASTEXITCODE" }

    $status = (& $Binary status | Out-String)
    if ($LASTEXITCODE -ne 0 -or $status -notmatch 'is stopped') {
        throw "unexpected stopped status: $status"
    }
}
finally {
    & $Binary stop | Out-Null
}
