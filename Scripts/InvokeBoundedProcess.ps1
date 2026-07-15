Set-StrictMode -Version Latest

function Invoke-BoundedProcess {
    param(
        [Parameter(Mandatory = $true)]
        [string]$FilePath,

        [string[]]$Arguments = @(),

        [Parameter(Mandatory = $true)]
        [string]$Label,

        [ValidateRange(1, 3600)]
        [int]$TimeoutSeconds = 180,

        [ValidateRange(1, 200)]
        [int]$LastOutputLineCount = 40
    )

    function Get-ProcessTreeIds([int]$RootProcessId) {
        $Processes = @(Get-CimInstance Win32_Process -ErrorAction SilentlyContinue)
        $Pending = [System.Collections.Generic.Queue[int]]::new()
        $Seen = [System.Collections.Generic.HashSet[int]]::new()
        $Pending.Enqueue($RootProcessId)
        [void]$Seen.Add($RootProcessId)
        while ($Pending.Count -gt 0) {
            $ParentProcessId = $Pending.Dequeue()
            foreach ($Child in $Processes | Where-Object { $_.ParentProcessId -eq $ParentProcessId }) {
                if ($Seen.Add([int]$Child.ProcessId)) {
                    $Pending.Enqueue([int]$Child.ProcessId)
                }
            }
        }
        return @($Seen)
    }

    function ConvertTo-WindowsCommandLineArgument([string]$Argument) {
        if ($Argument.Length -eq 0) {
            return '""'
        }
        if ($Argument -notmatch '[\s"]') {
            return $Argument
        }
        $Escaped = [regex]::Replace($Argument, '(\\*)"', '$1$1\\"')
        $Escaped = $Escaped -replace '(\\*)$', '$1$1'
        return '"' + $Escaped + '"'
    }

    function Save-ProcessDump([int]$ProcessId, [string]$InvocationLabel) {
        Write-Host "[$InvocationLabel] Process dump unavailable: no repository-admitted Windows dump utility is configured."
    }

    $Lines = [System.Collections.Generic.List[string]]::new()
    $Process = $null
    $StandardOutputReader = $null
    $StandardErrorReader = $null
    $TimedOut = $false
    $TreeIds = @()
    try {
        $StartInfo = [System.Diagnostics.ProcessStartInfo]::new()
        $StartInfo.FileName = $FilePath
        $StartInfo.UseShellExecute = $false
        $StartInfo.RedirectStandardOutput = $true
        $StartInfo.RedirectStandardError = $true
        $StartInfo.CreateNoWindow = $true
        $StartInfo.Arguments = (($Arguments | ForEach-Object { ConvertTo-WindowsCommandLineArgument $_ }) -join ' ')

        $Process = [System.Diagnostics.Process]::new()
        $Process.StartInfo = $StartInfo
        if (!$Process.Start()) {
            throw "[$Label] Failed to start child process: $FilePath"
        }

        $StandardOutputReader = $Process.StandardOutput
        $StandardErrorReader = $Process.StandardError
        $StandardOutputRead = $StandardOutputReader.ReadLineAsync()
        $StandardErrorRead = $StandardErrorReader.ReadLineAsync()
        $Stopwatch = [System.Diagnostics.Stopwatch]::StartNew()
        while (!$Process.HasExited) {
            foreach ($Read in @(
                [PSCustomObject]@{ Task = $StandardOutputRead; Stream = "StandardOutput" },
                [PSCustomObject]@{ Task = $StandardErrorRead; Stream = "StandardError" }
            )) {
                if ($Read.Task.IsCompleted) {
                    $Line = $Read.Task.GetAwaiter().GetResult()
                    if ($null -ne $Line) {
                        $Lines.Add($Line)
                        Write-Host $Line
                        if ($Read.Stream -eq "StandardOutput") {
                            $StandardOutputRead = $StandardOutputReader.ReadLineAsync()
                        } else {
                            $StandardErrorRead = $StandardErrorReader.ReadLineAsync()
                        }
                    }
                }
            }
            if ($Stopwatch.Elapsed.TotalSeconds -ge $TimeoutSeconds) {
                $TimedOut = $true
                break
            }
            Start-Sleep -Milliseconds 100
        }

        if ($TimedOut) {
            $TreeIds = Get-ProcessTreeIds $Process.Id
            Write-Host "[$Label] Timed out after $([Math]::Round($Stopwatch.Elapsed.TotalSeconds, 1)) seconds; PID=$($Process.Id)."
            Write-Host "[$Label] Last useful output lines:"
            foreach ($Line in $Lines | Select-Object -Last $LastOutputLineCount) {
                Write-Host "[$Label] $Line"
            }
            Save-ProcessDump $Process.Id $Label
            foreach ($TreeProcessId in $TreeIds | Sort-Object -Descending) {
                Stop-Process -Id $TreeProcessId -Force -ErrorAction SilentlyContinue
            }
            $Process.WaitForExit(10000) | Out-Null
            $RemainingProcessIds = @($TreeIds | Where-Object { Get-Process -Id $_ -ErrorAction SilentlyContinue })
            if ($RemainingProcessIds.Count -gt 0) {
                throw "[$Label] Timed-out process tree did not terminate: $($RemainingProcessIds -join ', ')."
            }
            Write-Host "[$Label] Timed-out process tree terminated: $($TreeIds -join ', ')."
        } else {
            $Process.WaitForExit()
        }

        foreach ($Stream in @(
            [PSCustomObject]@{ Task = $StandardOutputRead; Reader = $StandardOutputReader },
            [PSCustomObject]@{ Task = $StandardErrorRead; Reader = $StandardErrorReader }
        )) {
            $Read = $Stream.Task
            while ($true) {
                $Line = $Read.GetAwaiter().GetResult()
                if ($null -eq $Line) {
                    break
                }
                $Lines.Add($Line)
                Write-Host $Line
                $Read = $Stream.Reader.ReadLineAsync()
            }
        }
        return [PSCustomObject]@{
            Label = $Label
            Output = @($Lines)
            ExitCode = if ($TimedOut) { $null } else { $Process.ExitCode }
            TimedOut = $TimedOut
            ProcessId = $Process.Id
        }
    } finally {
        if ($StandardOutputReader) { $StandardOutputReader.Dispose() }
        if ($StandardErrorReader) { $StandardErrorReader.Dispose() }
        if ($Process) { $Process.Dispose() }
    }
}
