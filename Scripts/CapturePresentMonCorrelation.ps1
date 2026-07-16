param(
    [Parameter(Mandatory = $true)]
    [ValidateNotNullOrEmpty()]
    [string]$PresentMonPath,
    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[0-9A-Fa-f]{64}$')]
    [string]$ExpectedPresentMonSha256,
    [string]$EditorPath = "",
    [string]$OutputDirectory = "",
    [ValidateSet("D3D12", "Vulkan")]
    [string]$Backend = "D3D12",
    [ValidateSet("responsive", "inter-frame", "submission-gate")]
    [string]$Candidate = "inter-frame",
    [ValidateRange(30, 600)]
    [int]$TargetFramesPerSecond = 60,
    [ValidateNotNullOrEmpty()]
    [string]$PresentationMode = "unknown",
    [ValidateNotNullOrEmpty()]
    [string]$SyncMode = "unknown",
    [ValidateNotNullOrEmpty()]
    [string]$VrrMode = "unknown",
    [ValidateNotNullOrEmpty()]
    [string]$TearingMode = "unknown",
    [ValidateRange(1, 120)]
    [int]$ReadinessTimeoutSeconds = 30,
    [ValidateRange(1, 120)]
    [int]$CollectorReadyTimeoutSeconds = 30,
    [ValidateRange(1, 600)]
    [int]$EditorTimeoutSeconds = 180,
    [ValidateRange(1, 600)]
    [int]$PresentMonTimeoutSeconds = 180,
    [ValidateRange(1500, 10000)]
    [int]$CollectorSettleMilliseconds = 1500,
    [ValidateRange(1, 10000000)]
    [UInt64]$FinalQpcTolerance = 2000,

    # TEST-ONLY: production invocation rejects fake hooks unless this switch is present.
    [switch]$TestMode,
    [string]$TestEditorScriptPath = "",
    [string]$TestCollectorScriptPath = "",
    [ValidateSet("success", "stale-readiness", "failure", "timeout", "linger-after-release")]
    [string]$TestEditorBehavior = "success",
    [ValidateSet("success", "linger-after-capture", "bad-header", "failure-before-ready", "exit-during-settle", "timeout")]
    [string]$TestCollectorBehavior = "success",
    [ValidateRange(10, 1500)]
    [int]$TestCollectorSettleMilliseconds = 50,
    [ValidateSet("none", "success", "failure", "survives", "timeout")]
    [string]$TestExactSessionTeardown = "none",
    [string]$TestPresentMonVersion = "1.10.0"
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$Root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
$Joiner = Join-Path $PSScriptRoot "JoinPresentMonCorrelation.ps1"
$ExpectedHeader = "Application,ProcessID,SwapChainAddress,Runtime,SyncInterval,PresentFlags,Dropped,TimeInSeconds,msInPresentAPI,msBetweenPresents,AllowsTearing,PresentMode,msUntilRenderComplete,msUntilDisplayed,msBetweenDisplayChange,QPCTime"

if (-not ("SpiralPresentMonJob" -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;

public static class SpiralPresentMonJob
{
    const UInt32 JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE = 0x00002000;
    const Int32 JobObjectExtendedLimitInformation = 9;

    [StructLayout(LayoutKind.Sequential)]
    struct JOBOBJECT_BASIC_LIMIT_INFORMATION
    {
        public Int64 PerProcessUserTimeLimit, PerJobUserTimeLimit;
        public UInt32 LimitFlags;
        public UIntPtr MinimumWorkingSetSize, MaximumWorkingSetSize;
        public UInt32 ActiveProcessLimit;
        public IntPtr Affinity;
        public UInt32 PriorityClass, SchedulingClass;
    }

    [StructLayout(LayoutKind.Sequential)]
    struct IO_COUNTERS
    {
        public UInt64 ReadOperationCount, WriteOperationCount, OtherOperationCount;
        public UInt64 ReadTransferCount, WriteTransferCount, OtherTransferCount;
    }

    [StructLayout(LayoutKind.Sequential)]
    struct JOBOBJECT_EXTENDED_LIMIT_INFORMATION
    {
        public JOBOBJECT_BASIC_LIMIT_INFORMATION BasicLimitInformation;
        public IO_COUNTERS IoInfo;
        public UIntPtr ProcessMemoryLimit, JobMemoryLimit, PeakProcessMemoryUsed, PeakJobMemoryUsed;
    }

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern IntPtr CreateJobObject(IntPtr attributes, string name);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool SetInformationJobObject(IntPtr job, Int32 infoClass, IntPtr info, UInt32 length);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool AssignProcessToJobObject(IntPtr job, IntPtr process);
    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern bool CloseHandle(IntPtr handle);

    public static IntPtr CreateKillOnCloseJob()
    {
        IntPtr job = CreateJobObject(IntPtr.Zero, null);
        if (job == IntPtr.Zero) throw new Win32Exception(Marshal.GetLastWin32Error(), "CreateJobObject failed");
        var info = new JOBOBJECT_EXTENDED_LIMIT_INFORMATION();
        info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        int size = Marshal.SizeOf(info);
        IntPtr buffer = Marshal.AllocHGlobal(size);
        try
        {
            Marshal.StructureToPtr(info, buffer, false);
            if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, buffer, (UInt32)size))
                throw new Win32Exception(Marshal.GetLastWin32Error(), "SetInformationJobObject failed");
            return job;
        }
        catch { CloseHandle(job); throw; }
        finally { Marshal.FreeHGlobal(buffer); }
    }

    public static void Assign(IntPtr job, IntPtr process)
    {
        if (!AssignProcessToJobObject(job, process))
            throw new Win32Exception(Marshal.GetLastWin32Error(), "AssignProcessToJobObject failed");
    }
}
'@
}

function ConvertTo-WindowsCommandLineArgument([string]$Argument) {
    if ($Argument.Length -eq 0) { return '""' }
    if ($Argument -notmatch '[\s"]') { return $Argument }
    $escaped = [regex]::Replace($Argument, '(\\*)"', '$1$1\\"')
    $escaped = $escaped -replace '(\\*)$', '$1$1'
    return '"' + $escaped + '"'
}

function Write-Atomically([string]$Path, [string]$Text) {
    $directory = Split-Path -Parent $Path
    if (![string]::IsNullOrWhiteSpace($directory)) { [IO.Directory]::CreateDirectory($directory) | Out-Null }
    $temporary = "$Path.$([guid]::NewGuid().ToString('N')).tmp"
    try {
        [IO.File]::WriteAllText($temporary, $Text, [Text.UTF8Encoding]::new($false))
        Move-Item -LiteralPath $temporary -Destination $Path -Force
    } finally {
        Remove-Item -LiteralPath $temporary -Force -ErrorAction SilentlyContinue
    }
}

function Get-ProcessTreeIds([int]$RootProcessId) {
    $processes = @(Get-CimInstance Win32_Process -ErrorAction SilentlyContinue)
    $children = @{}
    foreach ($candidate in $processes) {
        $parent = [int]$candidate.ParentProcessId
        if (!$children.ContainsKey($parent)) { $children[$parent] = [Collections.Generic.List[int]]::new() }
        $children[$parent].Add([int]$candidate.ProcessId)
    }
    $seen = [Collections.Generic.HashSet[int]]::new()
    $pending = [Collections.Generic.Queue[int]]::new()
    $pending.Enqueue($RootProcessId)
    while ($pending.Count -gt 0) {
        $current = $pending.Dequeue()
        if (!$seen.Add($current)) { continue }
        if ($children.ContainsKey($current)) {
            foreach ($child in $children[$current]) { $pending.Enqueue($child) }
        }
    }
    return @($seen)
}

function Capture-ProcessTree($State) {
    if ($null -eq $State) { return }
    foreach ($processId in @(Get-ProcessTreeIds $State.Process.Id)) { [void]$State.OwnedProcessIds.Add([int]$processId) }
}

function Start-LoggedProcess([string]$FilePath, [string[]]$Arguments, [string]$Label, [string]$StdoutPath, [string]$StderrPath) {
    $startInfo = [Diagnostics.ProcessStartInfo]::new()
    $startInfo.FileName = $FilePath
    $startInfo.UseShellExecute = $false
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true
    $startInfo.CreateNoWindow = $true
    $startInfo.Arguments = (($Arguments | ForEach-Object { ConvertTo-WindowsCommandLineArgument $_ }) -join ' ')
    $process = [Diagnostics.Process]::new()
    $process.StartInfo = $startInfo
    if (!$process.Start()) { throw "[$Label] Failed to start child process: $FilePath" }
    $jobHandle = [IntPtr]::Zero
    try {
        $jobHandle = [SpiralPresentMonJob]::CreateKillOnCloseJob()
        [SpiralPresentMonJob]::Assign($jobHandle, $process.Handle)
    } catch {
        if ($jobHandle -ne [IntPtr]::Zero) { [void][SpiralPresentMonJob]::CloseHandle($jobHandle) }
        Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
        $process.Dispose()
        throw "[$Label] Could not place the child in its owned kill-on-close job: $($_.Exception.Message)"
    }
    $state = [pscustomobject]@{
        Label = $Label; Process = $process; Arguments = @($Arguments); StartedAt = [DateTimeOffset]::Now
        Stopwatch = [Diagnostics.Stopwatch]::StartNew(); OwnedProcessIds = [Collections.Generic.HashSet[int]]::new()
        JobHandle = $jobHandle; JobClosed = $false
        StdoutReader = $process.StandardOutput; StderrReader = $process.StandardError
        StdoutWriter = [IO.StreamWriter]::new($StdoutPath, $false, [Text.UTF8Encoding]::new($false))
        StderrWriter = [IO.StreamWriter]::new($StderrPath, $false, [Text.UTF8Encoding]::new($false))
        StdoutTask = $null; StderrTask = $null; StdoutEnded = $false; StderrEnded = $false
        StdoutLines = [Collections.Generic.List[string]]::new(); StderrLines = [Collections.Generic.List[string]]::new()
    }
    $state.StdoutTask = $state.StdoutReader.ReadLineAsync()
    $state.StderrTask = $state.StderrReader.ReadLineAsync()
    Capture-ProcessTree $state
    return $state
}

function Pump-ProcessOutput($State, [switch]$Drain) {
    if ($null -eq $State) { return }
    foreach ($streamName in @("Stdout", "Stderr")) {
        $endedName = "${streamName}Ended"
        $taskName = "${streamName}Task"
        $readerName = "${streamName}Reader"
        $writerName = "${streamName}Writer"
        $linesName = "${streamName}Lines"
        while (!$State.$endedName -and ($Drain -or $State.$taskName.IsCompleted)) {
            $line = $State.$taskName.GetAwaiter().GetResult()
            if ($null -eq $line) { $State.$endedName = $true; break }
            $State.$linesName.Add($line)
            $State.$writerName.WriteLine($line)
            $State.$writerName.Flush()
            Write-Host "[$($State.Label) $($streamName.ToLowerInvariant())] $line"
            $State.$taskName = $State.$readerName.ReadLineAsync()
        }
    }
}

function Stop-OwnedProcessTree($State) {
    if ($null -eq $State) { return @() }
    Capture-ProcessTree $State
    if (!$State.JobClosed -and $State.JobHandle -ne [IntPtr]::Zero) {
        $State.JobClosed = [SpiralPresentMonJob]::CloseHandle($State.JobHandle)
        if ($State.JobClosed) { $State.JobHandle = [IntPtr]::Zero }
    }
    try { $State.Process.WaitForExit(10000) | Out-Null } catch {}
    return @($State.OwnedProcessIds | Where-Object { Get-Process -Id $_ -ErrorAction SilentlyContinue })
}

function Close-ProcessState($State) {
    if ($null -eq $State) { return }
    try { if ($State.Process.HasExited) { Pump-ProcessOutput $State -Drain } } catch {}
    $State.StdoutWriter.Dispose(); $State.StderrWriter.Dispose()
    $State.StdoutReader.Dispose(); $State.StderrReader.Dispose(); $State.Process.Dispose()
}

function Wait-StableFile([string]$Path, [string]$Label) {
    if (!(Test-Path -LiteralPath $Path -PathType Leaf)) { throw "$Label was not produced: $Path" }
    $prior = ""
    for ($attempt = 0; $attempt -lt 20; ++$attempt) {
        $item = Get-Item -LiteralPath $Path
        $current = "$($item.Length):$($item.LastWriteTimeUtc.Ticks):$((Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash)"
        if ($current -eq $prior) { return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant() }
        $prior = $current
        Start-Sleep -Milliseconds 100
    }
    throw "$Label did not become stable: $Path"
}

function Read-Readiness([string]$Path, [Diagnostics.Stopwatch]$Deadline, $EditorState) {
    while ($Deadline.Elapsed.TotalSeconds -lt $ReadinessTimeoutSeconds) {
        Pump-ProcessOutput $EditorState
        Capture-ProcessTree $EditorState
        if ($EditorState.Process.HasExited) { throw "Editor exited before publishing attachment readiness" }
        if (Test-Path -LiteralPath $Path -PathType Leaf) {
            try { return (Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json) } catch { }
        }
        Start-Sleep -Milliseconds 25
    }
    throw "Editor attachment readiness timed out after $ReadinessTimeoutSeconds seconds"
}

function Get-IsElevated {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    return ([Security.Principal.WindowsPrincipal]::new($identity)).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Test-ExactEtwSession([string]$SessionName) {
    $logman = Join-Path $env:SystemRoot "System32\logman.exe"
    if (!(Test-Path -LiteralPath $logman -PathType Leaf)) { throw "Windows logman.exe is unavailable for PresentMon session readiness" }
    & $logman query -ets $SessionName *> $null
    return $LASTEXITCODE -eq 0
}

function Stop-OwnedExactPresentMonSession([string]$ToolPath, [string]$SessionName, [string]$OutputDirectory) {
    $state = Start-LoggedProcess $ToolPath @("-session_name", $SessionName, "-terminate_existing") "PresentMon session teardown" `
        (Join-Path $OutputDirectory "presentmon-session-teardown.stdout.log") (Join-Path $OutputDirectory "presentmon-session-teardown.stderr.log")
    $watch = [Diagnostics.Stopwatch]::StartNew()
    while (!$state.Process.HasExited -and $watch.Elapsed.TotalSeconds -lt 10) { Pump-ProcessOutput $state; Start-Sleep -Milliseconds 25 }
    if (!$state.Process.HasExited) { [void](Stop-OwnedProcessTree $state); throw "Exact owned PresentMon session teardown timed out" }
    Pump-ProcessOutput $state -Drain
    if ($state.Process.ExitCode -ne 0) { Close-ProcessState $state; throw "Exact owned PresentMon session teardown failed with exit code $($state.Process.ExitCode)" }
    Close-ProcessState $state
}

$presentMonCanonical = [IO.Path]::GetFullPath($PresentMonPath)
if (!(Test-Path -LiteralPath $presentMonCanonical -PathType Leaf)) { throw "PresentMon executable does not exist: $presentMonCanonical" }
$actualToolHash = (Get-FileHash -LiteralPath $presentMonCanonical -Algorithm SHA256).Hash.ToLowerInvariant()
if ($actualToolHash -cne $ExpectedPresentMonSha256.ToLowerInvariant()) { throw "PresentMon SHA-256 does not match the explicitly supplied diagnostic provenance" }
if (!(Test-Path -LiteralPath $Joiner -PathType Leaf)) { throw "PresentMon joiner is unavailable: $Joiner" }

if ($TestMode) {
    if ([string]::IsNullOrWhiteSpace($TestEditorScriptPath) -or [string]::IsNullOrWhiteSpace($TestCollectorScriptPath)) {
        throw "TestMode requires explicit fake editor and collector scripts"
    }
    foreach ($testScript in @($TestEditorScriptPath, $TestCollectorScriptPath)) {
        if (!(Test-Path -LiteralPath $testScript -PathType Leaf)) { throw "TestMode script is unavailable: $testScript" }
    }
} elseif (![string]::IsNullOrWhiteSpace($TestEditorScriptPath) -or ![string]::IsNullOrWhiteSpace($TestCollectorScriptPath) -or $TestExactSessionTeardown -ne "none") {
    throw "Fake process hooks require explicit TestMode"
}

if ([string]::IsNullOrWhiteSpace($EditorPath)) { $EditorPath = Join-Path $Root "bin\Debug-windows-x86_64\Editor\Editor.exe" }
$editorCanonical = [IO.Path]::GetFullPath($EditorPath)
if (!(Test-Path -LiteralPath $editorCanonical -PathType Leaf)) { throw "Editor executable does not exist: $editorCanonical" }

$captureId = "capture-$([DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss'))-$([guid]::NewGuid().ToString('N').Substring(0, 12))"
if ([string]::IsNullOrWhiteSpace($OutputDirectory)) { $OutputDirectory = Join-Path $Root "output\presentmon-correlation\$captureId" }
$outputCanonical = [IO.Path]::GetFullPath($OutputDirectory)
if (Test-Path -LiteralPath $outputCanonical) { throw "Capture output directory must not already exist: $outputCanonical" }
[IO.Directory]::CreateDirectory($outputCanonical) | Out-Null

$engineDirectory = Join-Path $outputCanonical "raw-engine"
$presentMonCsv = Join-Path $outputCanonical "raw-presentmon\presentmon.csv"
$reportPath = Join-Path $outputCanonical "report\presentmon-correlation.json"
$receiptPath = Join-Path $outputCanonical "capture-receipt.json"
$readinessPath = Join-Path $outputCanonical "readiness.json"
$releasePath = Join-Path $outputCanonical "release.txt"
[IO.Directory]::CreateDirectory($engineDirectory) | Out-Null
[IO.Directory]::CreateDirectory((Split-Path -Parent $presentMonCsv)) | Out-Null
$sessionName = "GameEngine-PM-$([guid]::NewGuid().ToString('N'))"

$editorState = $null
$collectorState = $null
$toolValidationState = $null
$readiness = $null
$toolVersion = $null
$collectorReadyAt = $null
$collectorSettledAt = $null
$releaseAt = $null
$failure = $null
$joined = $false
$success = $false
$editorTimedOut = $false
$collectorTimedOut = $false
$presentMonTerminationMode = "natural"
$cleanup = [ordered]@{
    editorSurvivors = @(); presentMonSurvivors = @(); toolValidationSurvivors = @()
    editorJobClosed = $false; presentMonJobClosed = $false; toolValidationJobClosed = $false; exactSessionCleanup = "not-created"; exactSessionHelperArguments = @(); verified = $false
}
$engineHash = $null
$presentMonHash = $null
$postCleanupPresentMonHash = $null
$reportHash = $null
$presentMonArguments = @()
$editorArguments = @()

try {
    if ($TestMode) {
        $toolVersion = $TestPresentMonVersion
    } else {
        $toolValidationState = Start-LoggedProcess $presentMonCanonical @("--help") "PresentMon version" `
            (Join-Path $outputCanonical "presentmon-version.stdout.log") (Join-Path $outputCanonical "presentmon-version.stderr.log")
        while (!$toolValidationState.Process.HasExited -and $toolValidationState.Stopwatch.Elapsed.TotalSeconds -lt 10) {
            Pump-ProcessOutput $toolValidationState; Capture-ProcessTree $toolValidationState; Start-Sleep -Milliseconds 25
        }
        if (!$toolValidationState.Process.HasExited) { throw "PresentMon version query timed out" }
        $toolValidationState.Process.WaitForExit(); Pump-ProcessOutput $toolValidationState -Drain
        $versionText = (@($toolValidationState.StdoutLines) + @($toolValidationState.StderrLines)) -join "`n"
        $versionMatch = [regex]::Match($versionText, '(?m)^PresentMon\s+([0-9]+\.[0-9]+\.[0-9]+)\s*$')
        if (!$versionMatch.Success) { throw "PresentMon executable did not report an exact semantic version" }
        $toolVersion = $versionMatch.Groups[1].Value
    }
    if ($toolVersion -cne "1.10.0") { throw "PresentMon version must be exactly 1.10.0; observed '$toolVersion'" }

    $attachmentTimeoutMilliseconds = [Math]::Min(60000, ($ReadinessTimeoutSeconds + $CollectorReadyTimeoutSeconds + 10) * 1000)
    if ($TestMode) {
        $editorArguments = @("-NoProfile", "-File", [IO.Path]::GetFullPath($TestEditorScriptPath), "-ReadinessPath", $readinessPath,
            "-ReleasePath", $releasePath, "-EngineDirectory", $engineDirectory, "-Behavior", $TestEditorBehavior,
            "-Backend", $Backend, "-Candidate", $Candidate, "-TargetFramesPerSecond", "$TargetFramesPerSecond")
    } else {
        $editorArguments = @("--frame-pacing-benchmark", "--smooth-frametime-target-fps=$TargetFramesPerSecond",
            "--frame-pacing-benchmark-output=$engineDirectory", "--frame-pacing-benchmark-attachment-readiness=$readinessPath",
            "--frame-pacing-benchmark-attachment-release=$releasePath", "--frame-pacing-benchmark-attachment-timeout-ms=$attachmentTimeoutMilliseconds",
            "--frame-pacing-benchmark-presentation=$PresentationMode", "--frame-pacing-benchmark-sync=$SyncMode",
            "--frame-pacing-benchmark-vrr=$VrrMode", "--frame-pacing-benchmark-tearing=$TearingMode")
        if ($Candidate -eq "responsive") { $editorArguments += "--frame-pacing-benchmark-responsive" }
        else { $editorArguments += "--smooth-frametime-candidate=$Candidate" }
        if ($Backend -eq "Vulkan") { $editorArguments += "--renderer-vulkan" }
    }
    $editorState = Start-LoggedProcess $editorCanonical $editorArguments "Editor" `
        (Join-Path $outputCanonical "editor.stdout.log") (Join-Path $outputCanonical "editor.stderr.log")
    $readiness = Read-Readiness $readinessPath ([Diagnostics.Stopwatch]::StartNew()) $editorState
    $expectedBackend = if ($Backend -eq "Vulkan") { "NVRHI Vulkan" } else { "NVRHI D3D12" }
    $expectedCandidate = if ($Candidate -eq "submission-gate") { "SubmissionGate" } else { "InterFrame" }
    if ($readiness.schema -ne 1 -or [string]::IsNullOrWhiteSpace([string]$readiness.runId) -or
        [int]$readiness.processId -ne $editorState.Process.Id -or
        ![IO.Path]::GetFullPath([string]$readiness.executablePath).Equals($editorCanonical, [StringComparison]::OrdinalIgnoreCase) -or
        [UInt64]$readiness.qpcFrequency -eq 0 -or [UInt64]$readiness.qpcTick -eq 0 -or
        ![IO.Path]::GetFullPath([string]$readiness.benchmarkArtifactPath).Equals($engineDirectory, [StringComparison]::OrdinalIgnoreCase) -or
        $readiness.condition.backend -cne $expectedBackend -or [double]$readiness.condition.targetFps -ne $TargetFramesPerSecond -or
        $readiness.condition.candidate -cne $expectedCandidate -or $readiness.condition.presentationMode -cne $PresentationMode -or
        $readiness.condition.sync -cne $SyncMode -or $readiness.condition.vrr -cne $VrrMode -or $readiness.condition.tearing -cne $TearingMode) {
        throw "Editor attachment readiness identity or condition metadata did not match the launched run"
    }
    if (Test-Path -LiteralPath (Join-Path $engineDirectory "frame-pacing-benchmark.json")) {
        throw "Engine benchmark artifact existed before collector readiness and supervisor release"
    }

    if ($TestMode) {
        $presentMonArguments = @("-NoProfile", "-File", [IO.Path]::GetFullPath($TestCollectorScriptPath), "-CsvPath", $presentMonCsv,
            "-ReadinessPath", $readinessPath, "-ReleasePath", $releasePath, "-EditorPid", "$($editorState.Process.Id)",
            "-SessionName", $sessionName, "-Behavior", $TestCollectorBehavior)
    } else {
        $presentMonArguments = @("-process_id", "$($readiness.processId)", "-output_file", $presentMonCsv, "-qpc_time",
            "-session_name", $sessionName, "-terminate_on_proc_exit", "-no_top")
    }
    $collectorState = Start-LoggedProcess $presentMonCanonical $presentMonArguments "PresentMon" `
        (Join-Path $outputCanonical "presentmon.stdout.log") (Join-Path $outputCanonical "presentmon.stderr.log")
    $collectorReadyWatch = [Diagnostics.Stopwatch]::StartNew()
    while ($collectorReadyWatch.Elapsed.TotalSeconds -lt $CollectorReadyTimeoutSeconds) {
        Pump-ProcessOutput $editorState; Pump-ProcessOutput $collectorState
        Capture-ProcessTree $editorState; Capture-ProcessTree $collectorState
        if ($collectorState.Process.HasExited) { throw "PresentMon exited before collector readiness" }
        if ($editorState.Process.HasExited) { throw "Editor exited before collector readiness" }
        $collectorReady = if ($TestMode) {
            ((@($collectorState.StdoutLines) + @($collectorState.StderrLines)) -contains "Started recording.")
        } else {
            Test-ExactEtwSession $sessionName
        }
        if ($collectorReady) {
            $collectorReadyAt = [DateTimeOffset]::Now
            break
        }
        Start-Sleep -Milliseconds 25
    }
    if ($null -eq $collectorReadyAt) { throw "PresentMon collector readiness timed out after $CollectorReadyTimeoutSeconds seconds" }
    if ($collectorState.Process.HasExited) { throw "PresentMon was not alive at the collector-ready boundary" }
    $effectiveCollectorSettleMilliseconds = if ($TestMode) { $TestCollectorSettleMilliseconds } else { $CollectorSettleMilliseconds }
    $collectorSettleWatch = [Diagnostics.Stopwatch]::StartNew()
    while ($collectorSettleWatch.ElapsedMilliseconds -lt $effectiveCollectorSettleMilliseconds) {
        Pump-ProcessOutput $editorState; Pump-ProcessOutput $collectorState
        Capture-ProcessTree $editorState; Capture-ProcessTree $collectorState
        if ($collectorState.Process.HasExited) { throw "PresentMon exited during collector settle" }
        if ($editorState.Process.HasExited) { throw "Editor exited during collector settle" }
        if (!$TestMode -and !(Test-ExactEtwSession $sessionName)) { throw "PresentMon exact ETW session disappeared during collector settle" }
        Start-Sleep -Milliseconds 25
    }
    if ($collectorState.Process.HasExited -or $editorState.Process.HasExited) { throw "Collector or Editor was not alive after collector settle" }
    if (!$TestMode -and !(Test-ExactEtwSession $sessionName)) { throw "PresentMon exact ETW session disappeared after collector settle" }
    $collectorSettledAt = [DateTimeOffset]::Now
    Write-Atomically $releasePath "schema=1`nrunId=$($readiness.runId)`npid=$($readiness.processId)`n"
    $releaseAt = [DateTimeOffset]::Now
    Write-Host "PresentMonSupervisorV1 state=released runId=$($readiness.runId) editorPid=$($readiness.processId) collectorPid=$($collectorState.Process.Id) session=$sessionName"

    while (!$editorState.Process.HasExited) {
        Pump-ProcessOutput $editorState; Pump-ProcessOutput $collectorState
        Capture-ProcessTree $editorState; Capture-ProcessTree $collectorState
        if (!$editorState.Process.HasExited -and $editorState.Stopwatch.Elapsed.TotalSeconds -ge $EditorTimeoutSeconds) {
            $editorTimedOut = $true; throw "Editor timed out after $EditorTimeoutSeconds seconds"
        }
        if (!$collectorState.Process.HasExited -and $collectorState.Stopwatch.Elapsed.TotalSeconds -ge $PresentMonTimeoutSeconds) {
            $collectorTimedOut = $true; throw "PresentMon timed out after $PresentMonTimeoutSeconds seconds"
        }
        # A collector may finish flushing while the launched Editor process is in its final teardown.
        # The post-Editor stable raw/header/join checks remain the acceptance boundary.
        if ($editorState.Process.HasExited -and $editorState.Process.ExitCode -ne 0) { throw "Editor failed with exit code $($editorState.Process.ExitCode)" }
        Start-Sleep -Milliseconds 25
    }
    $editorState.Process.WaitForExit()
    Pump-ProcessOutput $editorState -Drain; Pump-ProcessOutput $collectorState
    if ($editorState.Process.ExitCode -ne 0) { throw "Editor failed with exit code $($editorState.Process.ExitCode)" }

    $engineJson = Join-Path $engineDirectory "frame-pacing-benchmark.json"
    $engineHash = Wait-StableFile $engineJson "Engine raw JSON"
    $presentMonHash = Wait-StableFile $presentMonCsv "PresentMon raw CSV"
    $actualHeader = Get-Content -LiteralPath $presentMonCsv -TotalCount 1
    if (-not ($actualHeader -ceq $ExpectedHeader)) { throw "PresentMon CSV actual header does not match the required 1.10.0 header" }
    $engineArtifact = Get-Content -LiteralPath $engineJson -Raw | ConvertFrom-Json
    $expectedFrameCount = if ($TestMode) { 3 } else { 512 }
    if ($engineArtifact.schema -notin @(2, 3) -or $engineArtifact.frames.Count -ne $expectedFrameCount -or
        $engineArtifact.condition.runId -cne $readiness.runId -or [int]$engineArtifact.condition.processId -ne [int]$readiness.processId -or
        [UInt64]$engineArtifact.condition.qpcFrequency -ne [UInt64]$readiness.qpcFrequency) {
        throw "Stable engine artifact did not retain the released identity and expected frame count"
    }
    & $Joiner -EngineJsonPath $engineJson -PresentMonCsvPath $presentMonCsv -OutputPath $reportPath -FinalQpcTolerance $FinalQpcTolerance
    $reportHash = Wait-StableFile $reportPath "PresentMon correlation report"
    $report = Get-Content -LiteralPath $reportPath -Raw | ConvertFrom-Json
    if ($report.rawInputs.engineJsonSha256 -cne $engineHash -or $report.rawInputs.presentMonCsvSha256 -cne $presentMonHash -or
        $report.counts.pairedRows -ne $expectedFrameCount) { throw "Correlation report did not retain the stable raw inputs and one-to-one count" }
    $joined = $true
    if (!$collectorState.Process.HasExited) {
        $presentMonTerminationMode = "owned-job-after-complete-capture"
        $cleanup.presentMonSurvivors = @(Stop-OwnedProcessTree $collectorState)
        if ($cleanup.presentMonSurvivors.Count -ne 0) { throw "Owned PresentMon process tree did not terminate after complete capture" }
    } elseif ($collectorState.Process.ExitCode -ne 0) {
        throw "PresentMon failed with exit code $($collectorState.Process.ExitCode)"
    }
    $postCleanupPresentMonHash = (Get-FileHash -LiteralPath $presentMonCsv -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($postCleanupPresentMonHash -cne $presentMonHash) { throw "PresentMon raw CSV changed after owned collector cleanup" }
    $success = $true
} catch {
    $failure = $_.Exception.Message
} finally {
    $cleanup.editorSurvivors = @(Stop-OwnedProcessTree $editorState)
    $cleanup.presentMonSurvivors = @(Stop-OwnedProcessTree $collectorState)
    $cleanup.toolValidationSurvivors = @(Stop-OwnedProcessTree $toolValidationState)
    if ($TestMode -and $TestExactSessionTeardown -ne "none") {
        $cleanup.exactSessionHelperArguments = @("-session_name", $sessionName, "-terminate_existing")
        switch ($TestExactSessionTeardown) {
            "success" { $cleanup.exactSessionCleanup = "terminated" }
            "failure" { $cleanup.exactSessionCleanup = "failed: simulated helper exit 9" }
            "survives" { $cleanup.exactSessionCleanup = "survived" }
            "timeout" { $cleanup.exactSessionCleanup = "failed: simulated helper timeout" }
        }
    } elseif (!$TestMode -and (Test-ExactEtwSession $sessionName)) {
        $cleanup.exactSessionHelperArguments = @("-session_name", $sessionName, "-terminate_existing")
        try { Stop-OwnedExactPresentMonSession $presentMonCanonical $sessionName $outputCanonical; $cleanup.exactSessionCleanup = if (Test-ExactEtwSession $sessionName) { "survived" } else { "terminated" } }
        catch { $cleanup.exactSessionCleanup = "failed: $($_.Exception.Message)" }
    } elseif ($TestMode) { $cleanup.exactSessionCleanup = "fake-not-applicable" }
    $cleanup.editorJobClosed = if ($editorState) { $editorState.JobClosed } else { $true }
    $cleanup.presentMonJobClosed = if ($collectorState) { $collectorState.JobClosed } else { $true }
    $cleanup.toolValidationJobClosed = if ($toolValidationState) { $toolValidationState.JobClosed } else { $true }
    $cleanup.verified = $cleanup.editorJobClosed -and $cleanup.presentMonJobClosed -and $cleanup.toolValidationJobClosed -and
        $cleanup.editorSurvivors.Count -eq 0 -and $cleanup.presentMonSurvivors.Count -eq 0 -and $cleanup.toolValidationSurvivors.Count -eq 0 -and
        ($cleanup.exactSessionCleanup -eq "not-created" -or $cleanup.exactSessionCleanup -eq "fake-not-applicable" -or $cleanup.exactSessionCleanup -eq "terminated")
    if (!$cleanup.verified) {
        $cleanupFailure = "Owned process-tree or exact-session cleanup failed"
        $failure = if ($null -eq $failure) { $cleanupFailure } else { "$failure; $cleanupFailure" }
        $success = $false
    }
    $receipt = [ordered]@{
        schema = 1; success = $success; error = $failure; captureId = $captureId; sessionName = $sessionName
        timing = [ordered]@{
            editorStartedAt = if ($editorState) { $editorState.StartedAt.ToString("o") } else { $null }
            readinessPublishedAt = if ($readiness) { (Get-Item -LiteralPath $readinessPath).LastWriteTimeUtc.ToString("o") } else { $null }
            collectorStartedAt = if ($collectorState) { $collectorState.StartedAt.ToString("o") } else { $null }
            collectorReadyAt = if ($collectorReadyAt) { $collectorReadyAt.ToString("o") } else { $null }
            collectorSettleMilliseconds = if ($collectorReadyAt) { $effectiveCollectorSettleMilliseconds } else { $null }
            collectorSettledAt = if ($collectorSettledAt) { $collectorSettledAt.ToString("o") } else { $null }
            releasedAt = if ($releaseAt) { $releaseAt.ToString("o") } else { $null }
        }
        tool = [ordered]@{ path = $presentMonCanonical; version = $toolVersion; sha256 = $actualToolHash; elevated = Get-IsElevated; arguments = @($presentMonArguments) }
        identity = [ordered]@{
            runId = if ($readiness) { $readiness.runId } else { $null }
            editorPid = if ($editorState) { $editorState.Process.Id } else { $null }
            readinessPid = if ($readiness) { $readiness.processId } else { $null }
            editorPath = $editorCanonical; readinessPath = if ($readiness) { $readiness.executablePath } else { $null }
            qpcFrequency = if ($readiness) { $readiness.qpcFrequency } else { $null }
            readinessQpc = if ($readiness) { $readiness.qpcTick } else { $null }
            presentMonPid = if ($collectorState) { $collectorState.Process.Id } else { $null }
        }
        condition = [ordered]@{
            backend = $Backend; targetFps = $TargetFramesPerSecond; candidate = $Candidate
            presentationMode = $PresentationMode; sync = $SyncMode; vrr = $VrrMode; tearing = $TearingMode
            monitor = "unknown"; rtss = "unavailable"; fes = "unavailable"; inputLatency = "unavailable"; gpuHeadroom = "unavailable"
        }
        paths = [ordered]@{
            outputDirectory = $outputCanonical; engineJson = Join-Path $engineDirectory "frame-pacing-benchmark.json"
            presentMonCsv = $presentMonCsv; report = $reportPath; editorStdout = Join-Path $outputCanonical "editor.stdout.log"
            editorStderr = Join-Path $outputCanonical "editor.stderr.log"; presentMonStdout = Join-Path $outputCanonical "presentmon.stdout.log"
            presentMonStderr = Join-Path $outputCanonical "presentmon.stderr.log"
        }
        hashes = [ordered]@{ engineJsonSha256 = $engineHash; presentMonCsvSha256 = $presentMonHash; presentMonCsvSha256AfterCleanup = $postCleanupPresentMonHash; reportSha256 = $reportHash }
        process = [ordered]@{
            editorExitCode = if ($editorState -and $editorState.Process.HasExited) { $editorState.Process.ExitCode } else { $null }
            presentMonExitCode = if ($presentMonTerminationMode -eq "natural" -and $collectorState -and $collectorState.Process.HasExited) { $collectorState.Process.ExitCode } else { $null }
            presentMonTerminationMode = $presentMonTerminationMode; editorTimedOut = $editorTimedOut; presentMonTimedOut = $collectorTimedOut; joined = $joined; cleanup = $cleanup
        }
    }
    try { Write-Atomically $receiptPath (($receipt | ConvertTo-Json -Depth 12) + [Environment]::NewLine) }
    catch { if ($null -eq $failure) { $failure = "Capture receipt publication failed: $($_.Exception.Message)" }; $success = $false }
    Close-ProcessState $editorState; Close-ProcessState $collectorState; Close-ProcessState $toolValidationState
}

if (!$success) { throw "PresentMon supervisor failed: $failure. Diagnostics: $outputCanonical" }
Write-Host "PresentMonSupervisorV1 state=passed runId=$($readiness.runId) editorPid=$($readiness.processId) pairs=$($report.counts.pairedRows) cleanup=verified receipt=$receiptPath"
