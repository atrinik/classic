param(
    [Parameter(Mandatory = $true)]
    [string]$Package,
    [Parameter(Mandatory = $true)]
    [ValidatePattern("^[0-9a-fA-F]{40}$")]
    [string]$Revision
)

$ErrorActionPreference = "Stop"
$packagePath = (Resolve-Path -LiteralPath $Package).Path
$smokeRoot = Join-Path ([System.IO.Path]::GetTempPath()) (
    "atrinik-review-bundle-smoke-{0}" -f [System.Guid]::NewGuid()
)
$process = $null
$launcherProcess = $null
$launcherOutputTask = $null
$launcherErrorTask = $null
$launcherServer = $null
$launcherClient = $null
$launcherClientLog = $null
$bodySucceeded = $false
$clientCloseStarted = $false
$portProbe = [System.Net.Sockets.UdpClient]::new(0)
try {
    $serverPort = ([System.Net.IPEndPoint]$portProbe.Client.LocalEndPoint).Port
} finally {
    $portProbe.Dispose()
}

function Get-LauncherOutput($Task) {
    if ($null -eq $Task) {
        return ""
    }
    if (-not $Task.Wait(1000)) {
        return "<launcher output did not close>"
    }
    return $Task.Result
}

function Get-LauncherLogTail([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        return "<log missing>"
    }
    try {
        $Lines = @(Get-Content -LiteralPath $Path -Tail 40 -ErrorAction Stop)
        if ($Lines.Count -eq 0) {
            return "<log empty>"
        }
        return (@(
            $Lines | ForEach-Object {
                # Drop the rest of a credential-bearing line, including quoted
                # or whitespace-separated values. Never print terminal controls.
                $safeLine = $_ -replace "[\x00-\x1f\x7f-\x9f]", "?"
                $safeLine = $safeLine -replace "(?i)(password|passwd|secret|token|authorization|auth|bearer).*", "[redacted]"
                $safeLine = $safeLine -replace "(?i)https?://\S+", "[redacted-url]"
                $safeLine.Substring(0, [System.Math]::Min($safeLine.Length, 2048))
            }
        ) -join [System.Environment]::NewLine)
    } catch {
        return "<log unavailable>"
    }
}

# Retain only fixed lifecycle messages from the actual launcher log before its
# temporary directory is removed. The closed grammar excludes private payloads.
function Save-ClientLifecycleEvidence([string]$Path) {
    try {
        $lines = @("<client lifecycle log unavailable>")
        if ($Path -and (Test-Path -LiteralPath $Path -PathType Leaf)) {
            # Bound the read as well as the retained output. Oversize evidence
            # remains unavailable; it must never change smoke acceptance.
            $reader = [System.IO.StreamReader]::new([System.IO.File]::Open(
                $Path, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read,
                [System.IO.FileShare]::ReadWrite))
            try {
                $buffer = [char[]]::new(16777217)
                $count = $reader.ReadBlock($buffer, 0, $buffer.Length)
            } finally {
                $reader.Dispose()
            }
            if ($count -le 16777216) {
                $text = [string]::new($buffer, 0, $count)
                $pattern = "(?m)^\[[0-9]{4}/[0-9]{2}/[0-9]{2} [0-9]{2}:[0-9]{2}:[0-9]{2}\.[0-9]{6}\] INFO[ ]+\[(?:socket_command_player|main|Event_PollInputDevice|system_end):[0-9]{1,10}\] (?:Gameplay ready\.|First gameplay frame presented\.|Client (?:window close event received|quit event received|quit event polling completed|main loop exited|shutdown complete)\.|Client shutdown stage: (?:UI cleanup|network connection|directory workers|audio|settings and assets|image workers|image packs|asset requests|toolkit, GPU, window and SDL)\.)\r?$"
                $lines = @([regex]::Matches($text, $pattern) | Select-Object -First 32 |
                    ForEach-Object { $_.Value.TrimEnd([char]13) })
                if ($lines.Count -eq 0) { $lines = @("<client lifecycle markers unavailable>") }
            } else {
                $lines = @("<client lifecycle log exceeds 16 Mi-character read limit>")
            }
        }
        $evidence = Join-Path (Split-Path -Parent $packagePath) "review-client-lifecycle.log"
        @("revision=$Revision") + $lines | Set-Content -LiteralPath $evidence -Encoding utf8 -ErrorAction Stop
        Write-Host ("Client lifecycle evidence:" + [System.Environment]::NewLine +
            ($lines -join [System.Environment]::NewLine))
    } catch {
        # Evidence is best effort and cannot replace an acceptance failure.
        Write-Warning "Client lifecycle evidence could not be retained"
    }
}

# Read-only HWND evidence: never read titles or send messages from this probe.
# CloseMainWindow below remains the single normal close request.
function Get-ClientWindowSnapshot($Client) {
    try {
        if (-not ("AtrinikReviewWindowProbe" -as [type])) {
            Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.RegularExpressions;
public static class AtrinikReviewWindowProbe {
    private delegate bool EnumWindowsProc(IntPtr window, IntPtr parameter);
    [DllImport("user32.dll")]
    private static extern bool EnumWindows(EnumWindowsProc callback, IntPtr parameter);
    [DllImport("user32.dll")]
    private static extern uint GetWindowThreadProcessId(IntPtr window, out uint processId);
    [DllImport("user32.dll", CharSet = CharSet.Unicode, ExactSpelling = true)]
    private static extern int GetClassNameW(IntPtr window, StringBuilder name, int capacity);
    [DllImport("user32.dll")]
    private static extern bool IsWindowVisible(IntPtr window);
    [DllImport("user32.dll")]
    private static extern bool IsWindowEnabled(IntPtr window);
    [DllImport("user32.dll")]
    private static extern IntPtr GetWindow(IntPtr window, uint command);

    public static string Snapshot(int processId, IntPtr mainWindow) {
        var rows = new List<string>();
        int visited = 0;
        bool complete = EnumWindows(delegate(IntPtr window, IntPtr parameter) {
            if (++visited > 4096 || rows.Count >= 32) return false;
            uint owner;
            uint thread = GetWindowThreadProcessId(window, out owner);
            if (owner != (uint)processId) return true;
            var name = new StringBuilder(256);
            GetClassNameW(window, name, name.Capacity);
            rows.Add(String.Format(
                "hwnd={0} main={1} thread={2} class={3} visible={4} enabled={5} owner_hwnd={6}",
                window.ToInt64(), window == mainWindow, thread,
                Regex.Replace(name.ToString(), "[^A-Za-z0-9_.#-]", "?"),
                IsWindowVisible(window), IsWindowEnabled(window),
                GetWindow(window, 4).ToInt64())); // GW_OWNER
            return true;
        }, IntPtr.Zero);
        return String.Format("pid={0} main_hwnd={1} enumeration_complete={2}; {3}",
            processId, mainWindow.ToInt64(), complete, String.Join("; ", rows));
    }
}
'@ -ErrorAction Stop
        }
        if ($Client.HasExited) {
            return "client_exited=True"
        }
        return [AtrinikReviewWindowProbe]::Snapshot($Client.Id, $Client.MainWindowHandle)
    } catch {
        # A diagnostic must not change close semantics or replace its failure.
        return "<client window snapshot unavailable>"
    }
}

# Timeout-only observation of the owned main window thread. WM_NULL requests no
# application action; its single bounded probe never changes close acceptance.
function Get-ClientCloseTimeoutSnapshot($Client) {
    try {
        if (-not ("AtrinikReviewCloseTimeoutProbe" -as [type])) {
            Add-Type -TypeDefinition @'
using System;
using System.Diagnostics;
using System.Runtime.InteropServices;
public static class AtrinikReviewCloseTimeoutProbe {
    [DllImport("user32.dll")]
    private static extern uint GetWindowThreadProcessId(IntPtr window, out uint processId);
    [DllImport("user32.dll", ExactSpelling = true)]
    private static extern IntPtr SendMessageTimeoutW(IntPtr window, uint message,
        UIntPtr wParam, IntPtr lParam, uint flags, uint timeout, out UIntPtr result);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern IntPtr OpenThread(uint access, bool inherit, uint threadId);
    [DllImport("kernel32.dll")]
    private static extern uint GetProcessIdOfThread(IntPtr thread);
    [DllImport("kernel32.dll")]
    private static extern uint WaitForSingleObject(IntPtr handle, uint milliseconds);
    [DllImport("kernel32.dll")]
    private static extern bool CloseHandle(IntPtr handle);
    private delegate void WaitChainCallback(IntPtr session, UIntPtr context, uint status,
        IntPtr count, IntPtr nodes, IntPtr cycle);
    [DllImport("advapi32.dll", SetLastError = true)]
    private static extern IntPtr OpenThreadWaitChainSession(uint flags, WaitChainCallback callback);
    [DllImport("advapi32.dll", SetLastError = true)]
    private static extern bool GetThreadWaitChain(IntPtr session, UIntPtr context, uint flags,
        uint threadId, IntPtr count, IntPtr nodes, IntPtr cycle);
    [DllImport("advapi32.dll")]
    private static extern void CloseThreadWaitChainSession(IntPtr session);

    // ABI and async lifetime contracts:
    // https://learn.microsoft.com/windows/win32/api/wct/ns-wct-waitchain_node_info
    // https://learn.microsoft.com/windows/win32/api/wct/nf-wct-closethreadwaitchainsession
    // Windows SDK wct.h: two DWORD enums followed by an 8-byte-aligned union
    // containing WCHAR[128], LARGE_INTEGER and BOOL (280 bytes on Windows x64).
    // No object-name bytes are decoded. Only the thread arm is ever read.
    private const int NodeSize = 280, MaxNodes = 16;
    private static string FormatWaitChain(uint processId, uint status, IntPtr count,
        IntPtr nodes, IntPtr cycle) {
        if (status != 0) return "wct_error=" + status;
        int length = Marshal.ReadInt32(count);
        if (length < 1 || length > MaxNodes) return "wct_invalid_count";
        var rows = new System.Collections.Generic.List<string>();
        bool externalBoundary = false;
        string[] types = { "Unknown", "CriticalSection", "SendMessage", "Mutex", "Alpc",
            "Com", "ThreadWait", "ProcessWait", "Thread", "ComActivation", "Unknown",
            "SocketIo", "SmbIo" };
        string[] statuses = { "Unknown", "NoAccess", "Running", "Blocked", "PidOnly",
            "PidOnlyRpcss", "Owned", "NotOwned", "Abandoned", "Unknown", "Error" };
        for (int index = 0; index < length; ++index) {
            IntPtr node = IntPtr.Add(nodes, index * NodeSize);
            int type = Marshal.ReadInt32(node), state = Marshal.ReadInt32(node, 4);
            string row = "type=" + (type >= 0 && type < types.Length ? types[type] : "Unknown") +
                " status=" + (state >= 0 && state < statuses.Length ? statuses[state] : "Unknown");
            if (type == 8) { // WctThreadType
                if (unchecked((uint)Marshal.ReadInt32(node, 8)) != processId) {
                    rows.Add("external_thread_boundary");
                    externalBoundary = true;
                    break; // Retain no identities or later rows beyond the client boundary.
                }
                row += " thread=" + unchecked((uint)Marshal.ReadInt32(node, 12));
            }
            rows.Add(row);
        }
        // WCT may have traversed farther. Do not report its hidden node count or
        // attribute a cycle involving omitted nodes to the retained client rows.
        return "wct_nodes=" + rows.Count + " cycle=" +
            (externalBoundary ? "unavailable" : (Marshal.ReadInt32(cycle) != 0).ToString()) +
            "; " + String.Join("; ", rows);
    }

    private static string WaitChain(Process client, IntPtr mainWindow, uint mainThread) {
        if (IntPtr.Size != 8) return "wct_unsupported_architecture";
        // Pin the original Process object's handle, not a rediscovered PID. The
        // observer owns this reference even if its bounded caller returns early.
        int processId = client.Id;
        var processHandle = client.SafeHandle;
        bool retained = false;
        string result = "wct_unavailable";
        var observer = new System.Threading.Thread(delegate() {
            IntPtr thread = IntPtr.Zero, session = IntPtr.Zero;
            IntPtr count = IntPtr.Zero, nodes = IntPtr.Zero, cycle = IntPtr.Zero;
            WaitChainCallback callback = null;
            string callbackResult = "wct_callback_unavailable";
            using (var completed = new System.Threading.ManualResetEvent(false)) {
                try {
                    // QUERY_LIMITED_INFORMATION | SYNCHRONIZE; no suspend or write access.
                    thread = OpenThread(0x00100800, false, mainThread);
                    if (thread == IntPtr.Zero) {
                        result = "wct_thread_error=" + Marshal.GetLastWin32Error();
                        return;
                    }
                    count = Marshal.AllocHGlobal(4);
                    nodes = Marshal.AllocHGlobal(NodeSize * MaxNodes);
                    cycle = Marshal.AllocHGlobal(4);
                    Marshal.WriteInt32(count, MaxNodes);
                    Marshal.WriteInt32(cycle, 0);
                    callback = delegate(IntPtr handle, UIntPtr context, uint status,
                                        IntPtr observedCount, IntPtr observedNodes, IntPtr observedCycle) {
                        try {
                            callbackResult = FormatWaitChain((uint)processId, status, observedCount,
                                observedNodes, observedCycle);
                        } catch { callbackResult = "wct_callback_unavailable"; }
                        finally { completed.Set(); }
                    };
                    session = OpenThreadWaitChainSession(1, callback); // WCT_ASYNC_OPEN_FLAG
                    if (session == IntPtr.Zero) {
                        result = "wct_session_error=" + Marshal.GetLastWin32Error();
                        return;
                    }
                    uint owner;
                    // Held native handles prevent ID reuse; recheck HWND/thread
                    // ownership and both lifetimes immediately before the query.
                    if (GetWindowThreadProcessId(mainWindow, out owner) != mainThread ||
                        owner != (uint)processId || GetProcessIdOfThread(thread) != owner ||
                        WaitForSingleObject(processHandle.DangerousGetHandle(), 0) != 258 ||
                        WaitForSingleObject(thread, 0) != 258) {
                        result = "wct_owned_thread_changed";
                        return;
                    }
                    // Allow cross-process chain traversal and critical-section
                    // lookup from this external observer of the verified client.
                    // WCT may traverse beyond the client; the formatter retains
                    // rows only up to its first external-client thread boundary.
                    // Do not enable out-of-process COM server enumeration.
                    // https://learn.microsoft.com/windows/win32/api/wct/nf-wct-getthreadwaitchain
                    bool accepted = GetThreadWaitChain(session, UIntPtr.Zero, 0x1 | 0x4,
                        mainThread, count, nodes, cycle); // OUT_OF_PROC | OUT_OF_PROC_CS
                    int error = Marshal.GetLastWin32Error();
                    if (!accepted && error != 997) { // ERROR_IO_PENDING
                        result = "wct_query_error=" + error;
                        return;
                    }
                    result = completed.WaitOne(1000) ? callbackResult : "wct_query_timeout";
                } catch { result = "wct_unavailable"; }
                finally {
                    // Close cancels outstanding async work and waits for callbacks.
                    // Keep callback, buffers and event alive until it returns.
                    if (session != IntPtr.Zero) CloseThreadWaitChainSession(session);
                    GC.KeepAlive(callback);
                    if (cycle != IntPtr.Zero) Marshal.FreeHGlobal(cycle);
                    if (nodes != IntPtr.Zero) Marshal.FreeHGlobal(nodes);
                    if (count != IntPtr.Zero) Marshal.FreeHGlobal(count);
                    if (thread != IntPtr.Zero) CloseHandle(thread);
                    processHandle.DangerousRelease();
                }
            }
        });
        observer.IsBackground = true;
        try {
            processHandle.DangerousAddRef(ref retained);
            observer.Start();
        }
        catch {
            if (retained) processHandle.DangerousRelease();
            return "wct_observer_unavailable";
        }
        // Also bound cancellation/interop; a pending observer retains its own
        // resources, prints nothing later, and cannot hold up smoke containment.
        return observer.Join(1500) ? result : "wct_observer_timeout";
    }

    public static string Snapshot(Process client, IntPtr mainWindow) {
        int processId = client.Id;
        uint owner;
        uint mainThread = GetWindowThreadProcessId(mainWindow, out owner);
        if (mainWindow == IntPtr.Zero || mainThread == 0 || owner != (uint)processId)
            return "owned_main_window_unavailable";
        string state = "unavailable", wait = "unavailable";
        using (Process process = Process.GetProcessById(processId)) {
            // A cross-process window cannot share this observer's message queue.
            if (process.Id == Process.GetCurrentProcess().Id) return "invalid_observer_pid";
            ProcessThreadCollection threads = process.Threads;
            for (int index = 0; index < threads.Count && index < 4096; ++index) {
                using (ProcessThread thread = threads[index]) {
                    if ((uint)thread.Id != mainThread) continue;
                    try {
                        System.Diagnostics.ThreadState observed = thread.ThreadState;
                        state = observed.ToString();
                        wait = observed == System.Diagnostics.ThreadState.Wait
                            ? thread.WaitReason.ToString() : "not_waiting";
                    } catch { /* A thread may exit between observations. */ }
                    break;
                }
            }
        }
        // Recheck ownership immediately before the only message probe.
        uint currentThread = GetWindowThreadProcessId(mainWindow, out owner);
        if (owner != (uint)processId || currentThread != mainThread)
            return "owned_main_window_changed";
        UIntPtr result;
        // WM_NULL, SMTO_ABORTIFHUNG | SMTO_ERRORONEXIT, hard 250 ms timeout.
        bool acknowledged = SendMessageTimeoutW(mainWindow, 0, UIntPtr.Zero,
            IntPtr.Zero, 0x0002 | 0x0020, 250, out result) != IntPtr.Zero;
        return String.Format("pid={0} main_hwnd={1} thread={2} state={3} wait={4} wm_null_ack={5}",
            processId, mainWindow.ToInt64(), mainThread, state, wait, acknowledged) +
            "; " + WaitChain(client, mainWindow, mainThread);
    }
}
'@ -ErrorAction Stop
        }
        if ($Client.HasExited) {
            return "client_exited=True"
        }
        return [AtrinikReviewCloseTimeoutProbe]::Snapshot($Client, $Client.MainWindowHandle)
    } catch {
        return "<client close timeout snapshot unavailable>"
    }
}

function Write-CloseFailureDiagnostics {
    try {
        try {
            $launcherClient.Refresh()
            Write-Host ("Client windows after close failure: " +
                (Get-ClientWindowSnapshot $launcherClient))
        } catch {
            Write-Host "<client process refresh unavailable>"
        }
        foreach ($entry in @(
            @{ Label = "Client log tail"; Path = $launcherClientLog },
            @{ Label = "Server log tail"; Path = $launcherServerLog },
            @{ Label = "Launcher failure log tail"; Path = $launcherFailureLog },
            @{ Label = "Launcher progress log tail"; Path = $launcherProgressLog }
        )) {
            Write-Host ($entry.Label + ":" + [System.Environment]::NewLine +
                (Get-LauncherLogTail $entry.Path))
        }
    } catch {
        Write-Host "<close failure diagnostics unavailable>"
    }
}

try {
    Expand-Archive -LiteralPath $packagePath -DestinationPath $smokeRoot
    $packageRoots = @(Get-ChildItem -LiteralPath $smokeRoot -Directory)
    $rootFiles = @(Get-ChildItem -LiteralPath $smokeRoot -File)
    if ($packageRoots.Count -ne 1 -or $rootFiles.Count -ne 0) {
        throw "Review ZIP must contain exactly one top-level directory"
    }
    $reviewRoot = $packageRoots[0].FullName

    foreach ($obsolete in @("client", "server")) {
        if (Test-Path -LiteralPath (Join-Path $reviewRoot $obsolete)) {
            throw "Review bundle contains obsolete '$obsolete' subdirectory"
        }
    }
    $launchers = @(Get-ChildItem -LiteralPath $reviewRoot -File -Filter "*.bat")
    if ($launchers.Count -ne 1 -or $launchers[0].Name -ne "run-review.bat") {
        throw "Review bundle must contain exactly one root run-review.bat launcher"
    }

    $launcherPath = Join-Path $reviewRoot "run-review.ps1"
    $parseTokens = $null
    $parseErrors = $null
    [void][System.Management.Automation.Language.Parser]::ParseFile(
        $launcherPath,
        [ref]$parseTokens,
        [ref]$parseErrors
    )
    if ($parseErrors.Count -ne 0) {
        throw "run-review.ps1 failed native PowerShell parsing: $($parseErrors[0].Message)"
    }

    $manifestPath = Join-Path $reviewRoot "BUNDLE-MANIFEST.json"
    $manifest = Get-Content -Raw -LiteralPath $manifestPath | ConvertFrom-Json
    if ($manifest.revision -ne $Revision.ToLowerInvariant() -or $manifest.udp_port -ne 1731) {
        throw "Review manifest revision or UDP port does not match the requested bundle"
    }
    $manifestPaths = [System.Collections.Generic.HashSet[string]]::new(
        [System.StringComparer]::OrdinalIgnoreCase
    )
    foreach ($entry in $manifest.files) {
        if (-not $manifestPaths.Add([string]$entry.path)) {
            throw "Review manifest repeats path '$($entry.path)'"
        }
        $path = Join-Path $reviewRoot ([string]$entry.path)
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
            throw "Review manifest payload is missing: $($entry.path)"
        }
        $file = Get-Item -LiteralPath $path
        $hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $path).Hash.ToLowerInvariant()
        if ($file.Length -ne [long]$entry.size -or $hash -ne [string]$entry.sha256) {
            throw "Review manifest payload does not match: $($entry.path)"
        }
    }
    $payloads = @(Get-ChildItem -LiteralPath $reviewRoot -File -Recurse | Where-Object {
        $_.FullName -ne $manifestPath
    })
    if ($payloads.Count -ne $manifestPaths.Count) {
        throw "Review manifest does not cover every bundled payload"
    }

    $state = Join-Path $smokeRoot "server-data"
    Copy-Item -LiteralPath (Join-Path $reviewRoot "install_data") -Destination $state -Recurse
    New-Item -ItemType Directory -Force -Path (Join-Path $state "tmp") | Out-Null

    $startInfo = [System.Diagnostics.ProcessStartInfo]::new()
    $startInfo.FileName = Join-Path $reviewRoot "atrinik-server.exe"
    $startInfo.WorkingDirectory = $reviewRoot
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    $startInfo.RedirectStandardInput = $true
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true
    foreach ($name in @("HTTP_PROXY", "HTTPS_PROXY", "ALL_PROXY", "NO_PROXY")) {
        [void]$startInfo.Environment.Remove($name)
    }
    $startInfo.Environment["NO_PROXY"] = "127.0.0.1,localhost"
    foreach ($argument in @(
        "--datapath=$state",
        "--port_quic=$serverPort",
        "--network_stack=ipv4=127.0.0.1",
        "--server_public=false",
        "--port_mapping=off",
        "--stun_server=off",
        "--http_url=off",
        "--metaserver_publish_origin=http://127.0.0.1:9",
        "--metaserver_rendezvous_origin=http://127.0.0.1:9/v1/classic"
    )) {
        $startInfo.ArgumentList.Add($argument)
    }

    $process = [System.Diagnostics.Process]::new()
    $process.StartInfo = $startInfo
    if (-not $process.Start()) {
        throw "Could not launch the flat review-bundle server"
    }
    $errorTask = $process.StandardError.ReadToEndAsync()
    $lines = [System.Collections.Generic.List[string]]::new()
    $ready = $false
    $deadline = [System.DateTime]::UtcNow.AddSeconds(60)
    $lineTask = $process.StandardOutput.ReadLineAsync()
    while (-not $process.HasExited -and [System.DateTime]::UtcNow -lt $deadline) {
        if (-not $lineTask.Wait(1000)) {
            continue
        }
        $line = $lineTask.Result
        if ($null -eq $line) {
            break
        }
        $lines.Add($line)
        Write-Host $line
        if ($line -match "Server ready\. Waiting for connections") {
            $ready = $true
            break
        }
        $lineTask = $process.StandardOutput.ReadLineAsync()
    }
    if (-not $ready) {
        throw "Flat review-bundle server did not reach ready state within 60 seconds"
    }
    $endpoints = @(Get-NetUDPEndpoint -LocalPort $serverPort | Where-Object {
        $_.OwningProcess -eq $process.Id
    })
    if ($endpoints.Count -ne 1 -or $endpoints[0].LocalAddress -ne "127.0.0.1") {
        throw "Flat review-bundle server did not own its isolated UDP endpoint"
    }

    $remainderTask = $process.StandardOutput.ReadToEndAsync()
    $shutdownDeadline = [System.DateTime]::UtcNow.AddSeconds(30)
    $shutdownAttempts = 0
    while (-not $process.HasExited -and [System.DateTime]::UtcNow -lt $shutdownDeadline) {
        $shutdownAttempts++
        try {
            $process.StandardInput.WriteLine("shutdown")
            $process.StandardInput.Flush()
        } catch {
            if (-not $process.HasExited) {
                throw
            }
            break
        }
        if ($process.WaitForExit(1000)) {
            break
        }
    }
    $shutdownTimedOut = -not $process.HasExited
    $process.StandardInput.Close()
    if ($shutdownTimedOut) {
        try {
            $process.Kill($true)
        } catch {
            if (-not $process.HasExited) {
                throw
            }
        }
        if (-not $process.WaitForExit(10000)) {
            throw "Flat review-bundle server process tree did not exit after forced containment"
        }
    }
    if (-not $remainderTask.Wait(10000) -or -not $errorTask.Wait(10000)) {
        throw "Flat review-bundle server output did not close"
    }
    $remainder = $remainderTask.Result
    if ($remainder) {
        $lines.Add($remainder)
        Write-Host $remainder
    }
    $errorOutput = $errorTask.Result
    $output = $lines -join "`n"
    if ($errorOutput) {
        $output += "`nSTDERR:`n$errorOutput"
    }
    if ($shutdownTimedOut) {
        throw (
            "Flat review-bundle server did not shut down after $shutdownAttempts " +
            "graceful attempts within 30 seconds:`n$output"
        )
    }
    if ($process.ExitCode -ne 0) {
        throw "Flat review-bundle server exited with code $($process.ExitCode):`n$output"
    }
    if ($output -notmatch "Server saves complete; releasing resources\.") {
        throw "Flat review-bundle server did not report a clean shutdown"
    }

    $launcherStartInfo = [System.Diagnostics.ProcessStartInfo]::new()
    $launcherStartInfo.FileName = $env:ComSpec
    $launcherStartInfo.WorkingDirectory = $reviewRoot
    $launcherStartInfo.UseShellExecute = $false
    $launcherStartInfo.CreateNoWindow = $true
    $launcherStartInfo.ArgumentList.Add("/d")
    $launcherStartInfo.RedirectStandardOutput = $true
    $launcherStartInfo.RedirectStandardError = $true
    $launcherStartInfo.Environment["ATRINIK_REVIEW_NO_PAUSE"] = "1"
    $launcherStartInfo.ArgumentList.Add("/c")
    $launcherStartInfo.ArgumentList.Add("call")
    $launcherStartInfo.ArgumentList.Add($launchers[0].FullName)
    $launcherProcess = [System.Diagnostics.Process]::new()
    $launcherProcess.StartInfo = $launcherStartInfo
    if (-not $launcherProcess.Start()) {
        throw "Could not execute the user-facing run-review.bat launcher"
    }

    $launcherOutputTask = $launcherProcess.StandardOutput.ReadToEndAsync()
    $launcherErrorTask = $launcherProcess.StandardError.ReadToEndAsync()
    $launcherDeadline = [System.DateTime]::UtcNow.AddSeconds(120)
    $serverExecutable = Join-Path $reviewRoot "atrinik-server.exe"
    $clientExecutable = Join-Path $reviewRoot "atrinik.exe"
    $launcherServerLog = Join-Path (Join-Path $reviewRoot "server-data") "server.log"
    $launcherClientLog = Join-Path $reviewRoot "client.log"
    $launcherFailureLog = Join-Path $reviewRoot "launcher-failure.log"
    $launcherProgressLog = Join-Path $reviewRoot "launcher-progress.log"
    $launcherReady = $false
    $serverLogText = ""
    $clientLogText = ""
    while ([System.DateTime]::UtcNow -lt $launcherDeadline) {
        if ($launcherProcess.HasExited) {
            $launcherStdout = Get-LauncherOutput $launcherOutputTask
            $launcherStderr = Get-LauncherOutput $launcherErrorTask
            $launcherServerLogTail = Get-LauncherLogTail $launcherServerLog
            $launcherClientLogTail = Get-LauncherLogTail $launcherClientLog
            $launcherFailureLogTail = Get-LauncherLogTail $launcherFailureLog
            $launcherProgressLogTail = Get-LauncherLogTail $launcherProgressLog
            throw (
                "run-review.bat exited before login smoke completion with code " +
                "$($launcherProcess.ExitCode):" + [System.Environment]::NewLine +
                "Launcher stdout:" + [System.Environment]::NewLine + $launcherStdout +
                [System.Environment]::NewLine + "Launcher stderr:" +
                [System.Environment]::NewLine + $launcherStderr +
                [System.Environment]::NewLine + "Server log tail:" +
                [System.Environment]::NewLine + $launcherServerLogTail +
                [System.Environment]::NewLine + "Client log tail:" +
                [System.Environment]::NewLine + $launcherClientLogTail +
                [System.Environment]::NewLine + "Launcher failure log tail:" +
                [System.Environment]::NewLine + $launcherFailureLogTail +
                [System.Environment]::NewLine + "Launcher progress log tail:" +
                [System.Environment]::NewLine + $launcherProgressLogTail
            )
        }
        $launcherServers = @(Get-Process -Name "atrinik-server" -ErrorAction SilentlyContinue |
            Where-Object { $_.Path -eq $serverExecutable })
        $launcherClients = @(Get-Process -Name "atrinik" -ErrorAction SilentlyContinue |
            Where-Object { $_.Path -eq $clientExecutable })
        if ($launcherServers.Count -eq 1) {
            $launcherServer = $launcherServers[0]
        }
        if ($launcherClients.Count -eq 1) {
            $launcherClient = $launcherClients[0]
        }

        if ($null -ne $launcherServer -and $null -ne $launcherClient) {
            try {
                $launcherServer.Refresh()
                $launcherClient.Refresh()
                $launcherEndpoints = @(Get-NetUDPEndpoint -LocalPort 1731 |
                    Where-Object { $_.OwningProcess -eq $launcherServer.Id })
                $serverLogText = ""
                $clientLogText = ""
                if (Test-Path -LiteralPath $launcherServerLog) {
                    $serverLogText = Get-Content -Raw -LiteralPath $launcherServerLog
                }
                if (Test-Path -LiteralPath $launcherClientLog) {
                    $clientLogText = Get-Content -Raw -LiteralPath $launcherClientLog
                }
                $combinedLogText =
                    $serverLogText + [System.Environment]::NewLine + $clientLogText
                foreach ($diagnostic in
                    [System.Text.RegularExpressions.Regex]::Matches(
                        $combinedLogText,
                        "HTTP request origin=[^\r\n]*"
                    )) {
                    if ($diagnostic.Value -match "://") {
                        throw "cURL diagnostic unexpectedly retained a URL"
                    }
                    if ($diagnostic.Value -match "endpoint=(?:http|https)-remote$") {
                        throw "cURL diagnostic unexpectedly targeted a remote endpoint"
                    }
                    if ($diagnostic.Value -notmatch
                        "HTTP request origin=[A-Za-z0-9_.-]+ endpoint=(?:(?:http|https)-loopback)$") {
                        throw "cURL diagnostic was not bounded to an origin and endpoint class"
                    }
                }
                if (-not $launcherServer.HasExited -and -not $launcherClient.HasExited -and
                    $launcherEndpoints.Count -eq 1 -and
                    $launcherEndpoints[0].LocalAddress -eq "127.0.0.1" -and
                    $serverLogText -match "Server ready\. Waiting for connections" -and
                    $serverLogText -match "Connection .*: player .* logged in" -and
                    $clientLogText -match "Connection established to selected server\." -and
                    $clientLogText -match "Gameplay ready\.") {
                    $launcherReady = $true
                    break
                }
            } catch [System.Management.Automation.ItemNotFoundException] {
            } catch [System.IO.IOException] {
            }
        }
        Start-Sleep -Milliseconds 250
    }
    if (-not $launcherReady) {
        $launcherStdout = Get-LauncherOutput $launcherOutputTask
        $launcherStderr = Get-LauncherOutput $launcherErrorTask
        $launcherServerLogTail = Get-LauncherLogTail $launcherServerLog
        $launcherClientLogTail = Get-LauncherLogTail $launcherClientLog
        $launcherFailureLogTail = Get-LauncherLogTail $launcherFailureLog
        $launcherProgressLogTail = Get-LauncherLogTail $launcherProgressLog
        throw (
            "One-click launcher did not prove loopback login and gameplay readiness within " +
            "120 seconds:" + [System.Environment]::NewLine +
            "Launcher stdout:" + [System.Environment]::NewLine + $launcherStdout +
            [System.Environment]::NewLine + "Launcher stderr:" +
            [System.Environment]::NewLine + $launcherStderr +
            [System.Environment]::NewLine + "Server log tail:" +
            [System.Environment]::NewLine + $launcherServerLogTail +
            [System.Environment]::NewLine + "Client log tail:" +
            [System.Environment]::NewLine + $launcherClientLogTail +
            [System.Environment]::NewLine + "Launcher failure log tail:" +
            [System.Environment]::NewLine + $launcherFailureLogTail +
            [System.Environment]::NewLine + "Launcher progress log tail:" +
            [System.Environment]::NewLine + $launcherProgressLogTail
        )
    }

    $launcherClient.Refresh()
    if ($launcherClient.HasExited) {
        throw "One-click client exited before the graceful close request"
    }
    $clientCloseStarted = $true
    Write-Host ("Client windows before normal close: " +
        (Get-ClientWindowSnapshot $launcherClient))
    if (-not $launcherClient.CloseMainWindow()) {
        throw "Could not request a normal close of the one-click client window"
    }
    Write-Host "Client normal close request accepted; waiting up to 30 seconds."
    if (-not $launcherClient.WaitForExit(30000)) {
        Write-Host ("Client close timeout thread: " +
            (Get-ClientCloseTimeoutSnapshot $launcherClient))
        throw "One-click client did not exit after the normal close request"
    }
    $launcherClient.Refresh()
    # The launcher owns and validates the client Process object. A process
    # object rediscovered through Get-Process can expose no ExitCode after a
    # GUI close, so the launcher and batch exit codes below are authoritative.
    $launcherClientExitCode = $launcherClient.ExitCode
    if ($null -ne $launcherClientExitCode -and $launcherClientExitCode -ne 0) {
        throw "One-click client exited with code $launcherClientExitCode"
    }
    $launcherClientLogText = Get-Content -Raw -LiteralPath $launcherClientLog
    if ($launcherClientLogText -notmatch "Client shutdown complete\.") {
        throw "One-click client did not report a clean shutdown"
    }

    $launcherServer.Refresh()
    if (-not $launcherServer.HasExited -and -not $launcherServer.WaitForExit(45000)) {
        throw "One-click server did not exit after the client closed"
    }
    $launcherServer.Refresh()
    $launcherServerExitCode = $launcherServer.ExitCode
    if ($null -ne $launcherServerExitCode -and $launcherServerExitCode -ne 0) {
        throw "One-click server exited with code $launcherServerExitCode"
    }
    $launcherServerLogText = Get-Content -Raw -LiteralPath $launcherServerLog
    if ($launcherServerLogText -notmatch "Server saves complete; releasing resources\.") {
        throw "One-click server did not report a clean shutdown"
    }
    if (-not $launcherProcess.WaitForExit(60000)) {
        throw "run-review.bat did not return after the graceful review shutdown"
    }
    if ($launcherProcess.ExitCode -ne 0) {
        throw "run-review.bat exited with code $($launcherProcess.ExitCode)"
    }
    $bodySucceeded = $true
} catch {
    if ($clientCloseStarted) {
        # Collect while the exact owned processes and logs still exist, before
        # failure containment and temporary-state removal in finally.
        Write-CloseFailureDiagnostics
    }
    throw
} finally {
    Save-ClientLifecycleEvidence $launcherClientLog
    $cleanupFailures = [System.Collections.Generic.List[string]]::new()
    if ($null -ne $process) {
        try {
            $process.StandardInput.Close()
            if (-not $process.HasExited) {
                $process.Kill($true)
            }
            if (-not $process.WaitForExit(10000)) {
                $cleanupFailures.Add("Flat review-bundle server did not exit during cleanup")
            }
        } catch {
            Write-Warning "Could not contain flat review-bundle server: $_"
            $cleanupFailures.Add("Could not contain flat review-bundle server")
        } finally {
            $process.Dispose()
        }
    }
    foreach ($launched in @($launcherClient, $launcherServer)) {
        if ($null -eq $launched) { continue }
        try {
            if (-not $launched.HasExited) {
                $launched.Kill($true)
            }
            if (-not $launched.WaitForExit(10000)) {
                $cleanupFailures.Add("One-click review process did not exit during cleanup")
            }
        } catch {
            Write-Warning "Could not contain one-click review process: $_"
            $cleanupFailures.Add("Could not contain one-click review process")
        } finally {
            $launched.Dispose()
        }
    }
    if ($null -ne $launcherProcess) {
        try {
            if (-not $launcherProcess.HasExited) {
                $launcherProcess.Kill($true)
            }
            [void]$launcherProcess.WaitForExit(10000)
        } finally {
            $launcherProcess.Dispose()
        }
    }
    if (Test-Path -LiteralPath $smokeRoot) {
        for ($attempt = 1; $attempt -le 5; $attempt++) {
            try {
                Remove-Item -LiteralPath $smokeRoot -Recurse -Force
                break
            } catch {
                if ($attempt -lt 5) { Start-Sleep -Seconds 1 }
            }
        }
        if (Test-Path -LiteralPath $smokeRoot) {
            $cleanupFailures.Add("Could not remove flat review-bundle smoke directory")
        }
    }
    if ($bodySucceeded -and $cleanupFailures.Count -ne 0) {
        throw ($cleanupFailures -join "; ")
    }
}
