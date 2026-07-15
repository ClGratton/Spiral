param()

$ErrorActionPreference = "Stop"

. (Join-Path $PSScriptRoot "InvokeBoundedProcess.ps1")

$PowerShell = (Get-Process -Id $PID).Path
$Successful = Invoke-BoundedProcess -FilePath $PowerShell -Arguments @("-NoProfile", "-Command", "Write-Output 'bounded stdout'; [Console]::Error.WriteLine('bounded stderr'); exit 0") -Label "bounded-process-self-test-success" -TimeoutSeconds 10
if ($Successful.TimedOut -or $Successful.ExitCode -ne 0 -or
    $Successful.Output -notcontains "bounded stdout" -or $Successful.Output -notcontains "bounded stderr") {
    throw "Bounded-process success self-test did not preserve output or exit code."
}

$HangCommand = "`$child = Start-Process -FilePath '$PowerShell' -ArgumentList '-NoProfile', '-Command', 'Start-Sleep -Seconds 30' -PassThru; Write-Output ('child-pid=' + `$child.Id); while (`$true) { Start-Sleep -Seconds 1 }"
$TimedOut = Invoke-BoundedProcess -FilePath $PowerShell -Arguments @("-NoProfile", "-Command", $HangCommand) -Label "bounded-process-self-test-timeout" -TimeoutSeconds 2
if (!$TimedOut.TimedOut) {
    throw "Bounded-process timeout self-test did not time out."
}
if ($TimedOut.Output -notmatch '^child-pid=') {
    throw "Bounded-process timeout self-test did not capture the child-process diagnostic output."
}

Write-Host "Bounded process self-test passed: output/exit propagation and timeout tree termination."
