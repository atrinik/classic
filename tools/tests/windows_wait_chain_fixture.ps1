# Copyright 2026 The Atrinik Project
# SPDX-License-Identifier: GPL-2.0-or-later
# Compile the actual smoke observer and exercise its formatter without invoking
# any native Windows API. Run with PowerShell 7 on Linux or Windows.
param([switch]$Native, [switch]$NativeChild)
$ErrorActionPreference = "Stop"
if ($NativeChild) {
    if (-not $IsWindows) { throw "Native child requires Windows" }
    Add-Type -TypeDefinition @'
using System;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Threading;
[assembly: DefaultDllImportSearchPaths(DllImportSearchPath.System32)]
public static class AtrinikStackFixtureWindow {
    [DllImport("user32.dll", CharSet = CharSet.Unicode, ExactSpelling = true)]
    private static extern IntPtr CreateWindowExW(uint style, string className, string title,
        uint windowStyle, int x, int y, int width, int height, IntPtr parent,
        IntPtr menu, IntPtr instance, IntPtr parameter);
    [DllImport("user32.dll")]
    private static extern bool DestroyWindow(IntPtr window);
    [DllImport("kernel32.dll")]
    private static extern uint GetCurrentThreadId();
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, ExactSpelling = true)]
    private static extern IntPtr LoadLibraryExW(string name, IntPtr file, uint flags);
    [DllImport("kernel32.dll")]
    private static extern bool FreeLibrary(IntPtr module);
    public static void Run() {
        IntPtr window = CreateWindowExW(0, "STATIC", "", 0, 0, 0, 1, 1,
            IntPtr.Zero, IntPtr.Zero, IntPtr.Zero, IntPtr.Zero);
        if (window == IntPtr.Zero) throw new InvalidOperationException("fixture_window_failed");
        // Load only the public system image; do not create a GPU device or draw.
        IntPtr warp = LoadLibraryExW("d3d10warp.dll", IntPtr.Zero, 0x00000800);
        if (warp == IntPtr.Zero) throw new InvalidOperationException("fixture_warp_load_failed");
        uint busyId = 0;
        int stop = 0;
        using var ready = new ManualResetEventSlim(false);
        var busy = new Thread(() => {
            busyId = GetCurrentThreadId();
            ready.Set();
            var timer = Stopwatch.StartNew();
            while (Volatile.Read(ref stop) == 0 && timer.ElapsedMilliseconds < 25000)
                Thread.SpinWait(1000);
        });
        busy.Start();
        ready.Wait();
        try {
            Console.WriteLine(window.ToInt64() + ":" + busyId);
            Console.Out.Flush();
            // Keep both fixture threads alive until all snapshots finish. A
            // watchdog still bounds a failed/missing parent without a GPU.
            var release = System.Threading.Tasks.Task.Run(() => Console.ReadLine());
            if (!release.Wait(20000) || release.Result != "release")
                throw new InvalidOperationException("fixture_release_failed");
        } finally {
            Volatile.Write(ref stop, 1);
            busy.Join(2000);
            DestroyWindow(window);
            FreeLibrary(warp);
        }
    }
}
'@
    [AtrinikStackFixtureWindow]::Run()
    exit 0
}
$smoke = Join-Path $PSScriptRoot "../ci/smoke_windows_review_bundle.ps1"
$tokens = $null
$errors = $null
$ast = [System.Management.Automation.Language.Parser]::ParseFile(
    $smoke, [ref]$tokens, [ref]$errors)
if ($errors.Count -ne 0) { throw "Smoke script has PowerShell parse errors" }
foreach ($name in @("Get-ClientCpuSample", "Get-ClientCpuProgress")) {
    $definition = $ast.Find({ param($node)
        $node -is [System.Management.Automation.Language.FunctionDefinitionAst] -and
        $node.Name -eq $name
    }, $true)
    if ($null -eq $definition) { throw "Expected actual CPU diagnostic helper" }
    . ([scriptblock]::Create($definition.Extent.Text))
}
$cpuClient = [System.Diagnostics.Process]::GetCurrentProcess()
try {
    $baseline = Get-ClientCpuSample $cpuClient
    if ($null -eq $baseline) { throw "Actual owned-process CPU sample unavailable" }
    $progress = Get-ClientCpuProgress $cpuClient $baseline
    if ($progress -notmatch '^pid=[0-9]+ cpu_ms=[0-9]+ elapsed_ms=[0-9]+$') {
        throw "Owned-process CPU progress grammar mismatch"
    }
    foreach ($field in @("ProcessId", "Handle", "StartTicks", "CpuTicks", "ElapsedTicks")) {
        $changed = $baseline.PSObject.Copy()
        if ($field -eq "Handle") { $changed.$field = [IntPtr]::Zero }
        elseif ($field -in @("CpuTicks", "ElapsedTicks")) { $changed.$field = [long]::MaxValue }
        else { $changed.$field++ }
        if ((Get-ClientCpuProgress $cpuClient $changed) -cne '<client CPU progress unavailable>') {
            throw "CPU diagnostic accepted changed identity or regressed clock"
        }
    }
    if ((Get-ClientCpuProgress $cpuClient $null) -cne '<client CPU progress unavailable>') {
        throw "CPU diagnostic accepted missing baseline"
    }
} finally { $cpuClient.Dispose() }
if ($null -ne (Get-ClientCpuSample $cpuClient)) {
    throw "CPU diagnostic accepted a disposed owned process"
}
Write-Host "Actual owned-process CPU diagnostics and generation rejection fixtures passed."
$source = Get-Content -Raw -LiteralPath $smoke
$warpBlocks = @([regex]::Matches($source, "(?s)Add-Type -TypeDefinition @'\r?\n(.*?)\r?\n'@") |
    Where-Object { $_.Groups[1].Value.Contains("class AtrinikReviewWarpIdentityProbe") })
if ($warpBlocks.Count -ne 1) { throw "Expected one WARP identity observer" }
Add-Type -TypeDefinition $warpBlocks[0].Groups[1].Value
$peParser = [AtrinikReviewWarpIdentityProbe].GetMethod("Parse",
    [System.Reflection.BindingFlags]"NonPublic,Static")
function Invoke-WarpParser([byte[]]$Data, [bool]$Mapped = $false) {
    $reader = [Func[uint64,int,byte[]]]{
        param([uint64]$Offset, [int]$Count)
        $result = [byte[]]::new($Count)
        [Array]::Copy($Data, [long]$Offset, $result, 0, $Count)
        return ,$result
    }.GetNewClosure()
    return $peParser.Invoke($null, @($reader, $Mapped, [uint64]$Data.Length))
}
function Set-U32([byte[]]$Data, [int]$Offset, [uint32]$Value) {
    [BitConverter]::GetBytes($Value).CopyTo($Data, $Offset)
}
function Set-U16([byte[]]$Data, [int]$Offset, [uint16]$Value) {
    [BitConverter]::GetBytes($Value).CopyTo($Data, $Offset)
}
$pe = [byte[]]::new(2048)
Set-U16 $pe 0 0x5a4d
Set-U32 $pe 60 128
Set-U32 $pe 128 0x4550
Set-U16 $pe 132 0x8664
Set-U16 $pe 134 1
Set-U32 $pe 136 0x12345678
Set-U16 $pe 148 240
Set-U16 $pe 150 0x2000
Set-U16 $pe 152 0x20b
Set-U32 $pe 208 8192
Set-U32 $pe 260 16
Set-U32 $pe 312 4096
Set-U32 $pe 316 28
Set-U32 $pe 400 1024
Set-U32 $pe 404 4096
Set-U32 $pe 408 1024
Set-U32 $pe 412 512
$pdbPath = [Text.Encoding]::ASCII.GetBytes("C:\PRIVATE\build\d3d10warp.pdb" + [char]0)
Set-U32 $pe 524 2
Set-U32 $pe 528 (24 + $pdbPath.Length)
Set-U32 $pe 532 4128
Set-U32 $pe 536 544
Set-U32 $pe 544 0x53445352
([Guid]"00112233-4455-6677-8899-aabbccddeeff").ToByteArray().CopyTo($pe, 548)
Set-U32 $pe 564 1
$pdbPath.CopyTo($pe, 568)
$identity = Invoke-WarpParser $pe
if ($identity.Key -cne "12345678:00002000:00112233445566778899aabbccddeeff:1") {
    throw "PE identity or little-endian GUID mismatch"
}
$mapped = [byte[]]::new(8192)
[Array]::Copy($pe, 0, $mapped, 0, 512)
[Array]::Copy($pe, 512, $mapped, 4096, 1024)
if ((Invoke-WarpParser $mapped $true).Key -cne $identity.Key) {
    throw "Loaded-image RVA and disk-offset identities disagree"
}
foreach ($change in @(
    @(0, 0), @(60, 65537), @(132, 0), @(134, 97), @(148, 16), @(150, 0),
    @(152, 0), @(208, 67108865), @(260, 6), @(312, [uint32]::MaxValue),
    @(316, 4097), @(316, 27), @(524, 0), @(528, 24), @(528, 4097),
    @(532, [uint32]::MaxValue), @(536, 545), @(544, 0), @(564, 0), @(592, 0)
)) {
    $invalidPe = [byte[]]$pe.Clone()
    Set-U32 $invalidPe ([int]$change[0]) ([uint32]$change[1])
    $rejected = $false
    try { $null = Invoke-WarpParser $invalidPe } catch { $rejected = $true }
    if (-not $rejected) { throw "Malformed PE accepted at offset $($change[0])" }
}
foreach ($length in @(0, 63, 151, 391, 539, 570)) {
    $truncated = [byte[]]::new($length)
    [Array]::Copy($pe, $truncated, $length)
    $rejected = $false
    try { $null = Invoke-WarpParser $truncated } catch { $rejected = $true }
    if (-not $rejected) { throw "Truncated PE accepted" }
}
Write-Host "Actual WARP PE/RSDS parser, mapped/file identity, malformed and private-path fixtures passed."
# Exercise real artifact plumbing too: the async result must be retained without
# escaping through normal-success stdout. This fake completed observer has no IO.
$saveDefinition = $ast.Find({ param($node)
    $node -is [System.Management.Automation.Language.FunctionDefinitionAst] -and
    $node.Name -eq "Save-ClientLifecycleEvidence"
}, $true)
. ([scriptblock]::Create($saveDefinition.Extent.Text))
$artifactRoot = Join-Path ([IO.Path]::GetTempPath()) ("atrinik-warp-artifact-fixture-" + [Guid]::NewGuid())
$null = New-Item -ItemType Directory -Path $artifactRoot
try {
    $packagePath = Join-Path $artifactRoot "fixture.zip"
    $Revision = "a" * 40
    $clientWarpIdentityPending = [AtrinikReviewWarpIdentityProbe+Pending]::new()
    $resultField = $clientWarpIdentityPending.GetType().GetField("Result",
        [System.Reflection.BindingFlags]"NonPublic,Instance")
    $resultField.SetValue($clientWarpIdentityPending, "warp_identity_fixture")
    $output = & { Save-ClientLifecycleEvidence (Join-Path $artifactRoot "missing.log") } 6>&1 | Out-String
    $artifact = Get-Content -Raw (Join-Path $artifactRoot "review-client-lifecycle.log")
    if (-not $artifact.Contains("WARP module identity: warp_identity_fixture") -or
        $output.Contains("warp_identity_fixture") -or $output.Contains("WARP module identity:")) {
        throw "Private WARP artifact retention/stdout boundary failed"
    }
} finally { Remove-Item -LiteralPath $artifactRoot -Recurse -Force }
$fixtureSource = Get-Content -Raw -LiteralPath $PSCommandPath
$childBlocks = @([regex]::Matches($fixtureSource, "(?s)Add-Type -TypeDefinition @'\r?\n(.*?)\r?\n'@") |
    Where-Object { $_.Groups[1].Value.Contains("class AtrinikStackFixtureWindow") })
if ($childBlocks.Count -ne 1) { throw "Expected one native fixture child" }
Add-Type -TypeDefinition $childBlocks[0].Groups[1].Value
$blocks = @([regex]::Matches($source, "(?s)Add-Type -TypeDefinition @'\r?\n(.*?)\r?\n'@") |
    Where-Object { $_.Groups[1].Value.Contains("class AtrinikReviewCloseTimeoutProbe") })
if ($blocks.Count -ne 1) { throw "Expected one actual timeout observer" }
Add-Type -TypeDefinition $blocks[0].Groups[1].Value
$formatter = [AtrinikReviewCloseTimeoutProbe].GetMethod("FormatWaitChain",
    [System.Reflection.BindingFlags]"NonPublic,Static")
if ($null -eq $formatter) { throw "Expected wait-chain formatter" }
$count = [System.Runtime.InteropServices.Marshal]::AllocHGlobal(4)
$cycle = [System.Runtime.InteropServices.Marshal]::AllocHGlobal(4)
$nodes = [System.Runtime.InteropServices.Marshal]::AllocHGlobal(280 * 16)
try {
    [System.Runtime.InteropServices.Marshal]::Copy([byte[]]::new(280 * 16), 0, $nodes, 280 * 16)
    [System.Runtime.InteropServices.Marshal]::WriteInt32($count, 3)
    [System.Runtime.InteropServices.Marshal]::WriteInt32($cycle, 1)
    # Main thread -> named critical section -> another owned thread.
    foreach ($entry in @(
        @(0, 8), @(4, 3), @(8, 101), @(12, 202),
        @(280, 1), @(284, 6),
        @(560, 8), @(564, 2), @(568, 101), @(572, 303)
    )) {
        [System.Runtime.InteropServices.Marshal]::WriteInt32($nodes, $entry[0], $entry[1])
    }
    $privateName = [System.Text.Encoding]::Unicode.GetBytes("PRIVATE-OBJECT-NAME")
    [System.Runtime.InteropServices.Marshal]::Copy($privateName, 0,
        [IntPtr]::Add($nodes, 288), $privateName.Length)
    function Format-Chain([uint32]$Status = 0) {
        return $formatter.Invoke($null, @([uint32]101, $Status, $count, $nodes, $cycle))
    }
    $expected = "wct_nodes=3 cycle=True; type=Thread status=Blocked thread=202; " +
        "type=CriticalSection status=Owned; type=Thread status=Running thread=303"
    if ((Format-Chain) -cne $expected) { throw "Owned wait-chain fixture mismatch" }

    [System.Runtime.InteropServices.Marshal]::WriteInt32($nodes, 568, 404)
    [System.Runtime.InteropServices.Marshal]::WriteInt32($nodes, 572, 505)
    $expected = "wct_nodes=3 cycle=unavailable; type=Thread status=Blocked thread=202; " +
        "type=CriticalSection status=Owned; external_thread_boundary"
    if ((Format-Chain) -cne $expected) { throw "External identity boundary mismatch" }

    # Native traversal may continue beyond the first external thread, including
    # a later return to the client. Neither those rows nor aggregate information
    # about the omitted chain may escape the formatter boundary.
    [System.Runtime.InteropServices.Marshal]::WriteInt32($count, 5)
    foreach ($entry in @(
        @(840, 3), @(844, 6),
        @(1120, 8), @(1124, 3), @(1128, 101), @(1132, 606)
    )) {
        [System.Runtime.InteropServices.Marshal]::WriteInt32($nodes, $entry[0], $entry[1])
    }
    [System.Runtime.InteropServices.Marshal]::Copy($privateName, 0,
        [IntPtr]::Add($nodes, 848), $privateName.Length)
    if ((Format-Chain) -cne $expected) { throw "Post-boundary rows or count escaped" }
    [System.Runtime.InteropServices.Marshal]::WriteInt32($cycle, 0)
    if ((Format-Chain) -cne $expected) { throw "Post-boundary cycle escaped" }

    [System.Runtime.InteropServices.Marshal]::WriteInt32($count, 1)
    [System.Runtime.InteropServices.Marshal]::WriteInt32($cycle, 0)
    [System.Runtime.InteropServices.Marshal]::WriteInt32($nodes, 0, 2147483647)
    [System.Runtime.InteropServices.Marshal]::WriteInt32($nodes, 4, -1)
    if ((Format-Chain) -cne "wct_nodes=1 cycle=False; type=Unknown status=Unknown") {
        throw "Unknown enum fixture mismatch"
    }
    foreach ($invalidCount in @(0, 17, 2147483647, -1)) {
        [System.Runtime.InteropServices.Marshal]::WriteInt32($count, $invalidCount)
        if ((Format-Chain) -cne "wct_invalid_count") { throw "Node bound fixture mismatch" }
    }
    # Error callbacks need not provide valid result pointers. Never dereference them.
    foreach ($status in @(5, 50, 234, 1223)) {
        $observed = $formatter.Invoke($null, @([uint32]101, [uint32]$status,
            [IntPtr]::Zero, [IntPtr]::Zero, [IntPtr]::Zero))
        if ($observed -cne "wct_error=$status") { throw "Error callback fixture mismatch" }
    }
    Write-Host "PowerShell parse, actual C# compilation and wait-chain formatter fixtures passed."
    Write-Host "Native WCT APIs were not invoked; Windows execution remains required."
} finally {
    [System.Runtime.InteropServices.Marshal]::FreeHGlobal($nodes)
    [System.Runtime.InteropServices.Marshal]::FreeHGlobal($cycle)
    [System.Runtime.InteropServices.Marshal]::FreeHGlobal($count)
}

# Compile the actual stack observer too. Pure formatting/range fixtures require
# no Windows API, and never manufacture acceptance for native stack unwinding.
$stackBlocks = @([regex]::Matches($source, "(?s)Add-Type -TypeDefinition @'\r?\n(.*?)\r?\n'@") |
    Where-Object { $_.Groups[1].Value.Contains("class AtrinikReviewCloseStackProbe") })
if ($stackBlocks.Count -ne 1) { throw "Expected one stack observer" }
Add-Type -TypeDefinition $stackBlocks[0].Groups[1].Value
$stackFormatter = [AtrinikReviewCloseStackProbe].GetMethod("FormatAddress",
    [System.Reflection.BindingFlags]"NonPublic,Static")
foreach ($case in @(
    @("atrinik.exe", 4096, 8192, 4112, "atrinik.exe+0x10"),
    @("C:\private\SDL3.dll", 4096, 8192, 4112, "SDL3.dll+0x10"),
    @("/private/name`n.dll", 4096, 8192, 4112, "name?.dll+0x10"),
    @("private.dll", 4096, 8192, 4095, "unknown_module"),
    @("private.dll", 4096, 8192, 12288, "unknown_module")
)) {
    $actual = $stackFormatter.Invoke($null, @([string]$case[0], [uint64]$case[1],
        [uint64]$case[2], [uint64]$case[3]))
    if ($actual -cne $case[4]) { throw "Stack address privacy/boundary fixture mismatch" }
}
$contains = [AtrinikReviewCloseStackProbe].GetMethod("Contains",
    [System.Reflection.BindingFlags]"NonPublic,Static")
foreach ($case in @(
    @(([uint64]::MaxValue - 16), 16, ([uint64]::MaxValue - 8), 8, $true),
    @(([uint64]::MaxValue - 16), 16, ([uint64]::MaxValue - 8), 9, $false),
    @(4096, 32, 4096, 33, $false), @(4096, 32, 4095, 1, $false)
)) {
    $actual = $contains.Invoke($null, @([uint64]$case[0], [uint64]$case[1],
        [uint64]$case[2], [uint64]$case[3]))
    if ($actual -ne $case[4]) { throw "Stack range overflow fixture mismatch" }
}
Write-Host "Actual stack observer C# compilation, privacy and overflow fixtures passed."

# Exercise the actual ranking logic without Win32: TID alone must never let a
# reused thread, changed handle or regressed CPU counter enter the top two.
$readingType = [AtrinikReviewCloseStackProbe].GetNestedType("CpuReading", "NonPublic")
$listType = [System.Collections.Generic.List``1].MakeGenericType($readingType)
$rank = [AtrinikReviewCloseStackProbe].GetMethod("RankCpu", [System.Reflection.BindingFlags]"NonPublic,Static")
function New-CpuReadings($Rows) {
    $list = [Activator]::CreateInstance($listType)
    foreach ($row in $Rows) {
        $reading = [Activator]::CreateInstance($readingType, $true)
        $reading.Id = [uint32]$row[0]
        $reading.Created = [long]$row[1]
        $reading.Cpu = [long]$row[2]
        $reading.Handle = [IntPtr]::new([long]$row[3])
        $list.Add($reading)
    }
    return ,$list
}
$before = New-CpuReadings @(@(10, 100, 1000, 50), @(20, 200, 1000, 60), @(30, 300, 1000, 70))
$after = New-CpuReadings @(@(30, 300, 5000, 70), @(10, 100, 2000, 50), @(20, 200, 5000, 60))
$ranked = $rank.Invoke($null, @($before, $after))
if ($ranked.Count -ne 2 -or $ranked[0].Id -ne 20 -or $ranked[1].Id -ne 30 -or
    $ranked[0].Cpu -ne 4000) { throw "CPU delta/top-two/tie ranking mismatch" }
foreach ($row in @(@(10, 101, 5000, 50), @(10, 100, 5000, 51),
                    @(10, 100, 999, 50), @(10, 100, 1000, 50), @(99, 100, 5000, 50))) {
    $rejected = New-CpuReadings @($row)
    if ($rank.Invoke($null, @($before, $rejected)).Count -ne 0) {
        throw "CPU rank accepted changed generation/handle/counter or missing baseline"
    }
}
$large = New-CpuReadings @(1..257 | ForEach-Object { ,@($_, 100, 5000, $_) })
if ($rank.Invoke($null, @($before, $large)).Count -ne 0) { throw "CPU thread limit escaped" }
if ([AtrinikReviewCloseStackProbe]::CpuProgress($null, $null) -cne "thread_cpu_baseline_unavailable") {
    throw "Missing CPU baseline was accepted"
}
Write-Host "Actual CPU ranking, generation, handle, regression, tie and cap fixtures passed."
$suspensionType = [AtrinikReviewCloseStackProbe].GetNestedType("Suspension", "NonPublic")
$cancelled = [Activator]::CreateInstance($suspensionType, $true)
$cancelled.Cancel()
if ($cancelled.Begin([IntPtr]::Zero)) { throw "Cancelled observer attempted a late suspension" }

if ($Native) {
    if (-not $IsWindows -or [IntPtr]::Size -ne 8) { throw "Native stack fixture requires Windows x64" }
    $start = [System.Diagnostics.ProcessStartInfo]::new()
    $start.FileName = (Get-Process -Id $PID).Path
    foreach ($argument in @("-NoProfile", "-File", $PSCommandPath, "-NativeChild")) {
        $start.ArgumentList.Add($argument)
    }
    $start.UseShellExecute = $false
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    $start.RedirectStandardInput = $true
    $child = [System.Diagnostics.Process]::new()
    $child.StartInfo = $start
    try {
        if (-not $child.Start()) { throw "Native stack fixture child failed to start" }
        $line = $child.StandardOutput.ReadLineAsync()
        if (-not $line.Wait(10000) -or $line.Result -notmatch "^([1-9][0-9]{0,18}):([1-9][0-9]{0,9})$") {
            throw "Native stack fixture did not create an owned window"
        }
        $window = [IntPtr]::new([long]$Matches[1])
        $busyThread = [uint32]$Matches[2]
        $warp = @($child.Modules | Where-Object { $_.ModuleName -ieq "d3d10warp.dll" })
        if ($warp.Count -ne 1) { throw "Native fixture WARP image unavailable" }
        $warpAddress = [uint64]$warp[0].BaseAddress.ToInt64()
        $warpSize = [uint64]$warp[0].ModuleMemorySize
        $started = $child.StartTime.ToUniversalTime().Ticks
        $identity = [AtrinikReviewWarpIdentityProbe]::Snapshot($child, $child.Id, $started, $warpAddress, $warpSize)
        if ($identity -notmatch '^module=d3d10warp.dll sha256=[0-9a-f]{64} timestamp=[0-9a-f]{8} image_size=[0-9a-f]{8} pdb=d3d10warp.pdb guid=[0-9a-f]{32} age=[0-9a-f]{1,8}$') {
            throw "Native loaded WARP identity mismatch: $identity"
        }
        $duplicate = [System.Diagnostics.Process]::GetProcessById($child.Id)
        try { $pendingIdentity = [AtrinikReviewWarpIdentityProbe]::StartLoaded($duplicate) }
        finally { $duplicate.Dispose() }
        if ($pendingIdentity.Finish() -cne $identity) {
            throw "Asynchronous WARP identity/retained handle after caller disposal differed"
        }
        foreach ($changed in @(
            @($child.Id + 1, $started, $warpAddress, $warpSize),
            @($child.Id, $started + 1, $warpAddress, $warpSize),
            @($child.Id, $started, $warpAddress + 1, $warpSize),
            @($child.Id, $started, $warpAddress, $warpSize + 1)
        )) {
            $invalidIdentity = [AtrinikReviewWarpIdentityProbe]::Snapshot($child, [int]$changed[0],
                [long]$changed[1], [uint64]$changed[2], [uint64]$changed[3])
            if ($invalidIdentity -notin @("warp_identity_wrong_process", "warp_identity_unavailable")) {
                throw "Native WARP identity accepted changed process/module generation"
            }
        }
        $threadBaseline = [AtrinikReviewCloseStackProbe]::CaptureCpuBaseline($child)
        if ($null -eq $threadBaseline) { throw "Native thread CPU baseline unavailable" }
        try {
            $readingsField = $threadBaseline.GetType().GetField("readings",
                [System.Reflection.BindingFlags]"NonPublic,Instance")
            $busyReading = @($readingsField.GetValue($threadBaseline) |
                Where-Object { $_.Id -eq $busyThread })
            if ($busyReading.Count -ne 1) { throw "Native busy thread generation unavailable" }
            $captureThread = [AtrinikReviewCloseStackProbe].GetMethod("SnapshotThread",
                [System.Reflection.BindingFlags]"NonPublic,Static")
            $changed = $captureThread.Invoke($null, @($child, [IntPtr]::Zero,
                $busyThread, ([long]$busyReading[0].Created + 1)))
            if ($changed -cne "stack_thread_generation_changed") {
                throw "Native stack observer accepted a changed thread generation"
            }
            Start-Sleep -Milliseconds 500
            $cpuRanked = [AtrinikReviewCloseStackProbe]::CpuProgress($child, $threadBaseline)
            if ($cpuRanked -notmatch "^thread_cpu_samples=[1-9][0-9]{0,2} baseline=[1-9][0-9]{0,2} elapsed_ms=[0-9]+ ranked=[12]; thread=$busyThread cpu_ms=[1-9][0-9]*; stack_frames=(?:[1-9]|[12][0-9]|3[0-2]); ") {
                throw "Native busy thread was not CPU-ranked and captured: $cpuRanked"
            }
        } finally { $threadBaseline.Dispose() }
        if ([AtrinikReviewCloseStackProbe]::CpuProgress($child, $threadBaseline) -cne "thread_cpu_generation_unavailable") {
            throw "Disposed thread baseline was accepted"
        }
        $invalid = [AtrinikReviewCloseStackProbe]::Snapshot($child, [IntPtr]::Zero)
        if ($invalid -cne "stack_owned_window_unavailable") { throw "Invalid window was accepted" }
        $observed = [AtrinikReviewCloseStackProbe]::Snapshot($child, $window)
        if ($observed -notmatch "^stack_frames=(?:[2-9]|[12][0-9]|3[0-2]); [A-Za-z0-9_.?-]+\+0x[0-9a-f]+(?:; (?:[A-Za-z0-9_.?-]+\+0x[0-9a-f]+|unknown_module))+$") {
            throw "Native stack fixture did not yield bounded module-relative frames: $observed"
        }
        # Natural completion proves the one owned suspend count was removed.
        $child.StandardInput.WriteLine("release")
        $child.StandardInput.Flush()
        if (-not $child.WaitForExit(10000) -or $child.ExitCode -ne 0) {
            throw "Native stack fixture child did not resume and exit normally"
        }
        Write-Host "Native owned-window stack capture and natural child exit passed: $observed"
        Write-Host "Native owned CPU-busy worker ranking and bounded stack passed: $cpuRanked"
        Write-Host "Native loaded System32 WARP identity and generation fences passed: $identity"
    } finally {
        if ($child.Id -and -not $child.HasExited) {
            $child.Kill($true)
            $null = $child.WaitForExit(5000)
        }
        $child.Dispose()
    }
}
