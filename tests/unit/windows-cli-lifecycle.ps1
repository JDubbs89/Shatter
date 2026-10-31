param([Parameter(Mandatory = $true)][string]$Binary)
$ErrorActionPreference = 'Stop'

try {
    & $Binary start | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "shatter start exited with $LASTEXITCODE" }

    $status = (& $Binary status | Out-String)
    if ($LASTEXITCODE -ne 0 -or $status -notmatch 'is running; audio-unavailable:.*no audio backend is configured') {
        throw "unexpected running status: $status"
    }

    $inputs = (& $Binary list in | Out-String)
    if ($LASTEXITCODE -ne 0 -or $inputs -notmatch 'no audio backend is configured') {
        throw "unexpected input listing: $inputs"
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
