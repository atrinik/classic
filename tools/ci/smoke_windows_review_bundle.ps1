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
$clientThreadCpuBaseline = $null
$clientWarpIdentityPending = $null
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

# Observe only the retained owned Process handle. PID reuse, disposal, exit and
# counters moving backwards make this evidence unavailable, never successful.
function Get-ClientCpuSample($Client) {
    try {
        $Client.Refresh()
        $handle = $Client.SafeHandle
        if ($Client.HasExited -or $handle.IsClosed -or $handle.IsInvalid) { return $null }
        $retained = $false
        try {
            $handle.DangerousAddRef([ref]$retained)
            $sample = [pscustomobject]@{
                ProcessId = $Client.Id
                Handle = $handle.DangerousGetHandle()
                StartTicks = $Client.StartTime.ToUniversalTime().Ticks
                CpuTicks = $Client.TotalProcessorTime.Ticks
                ElapsedTicks = [System.Environment]::TickCount64
            }
            if ($Client.HasExited) { return $null }
            return $sample
        } finally {
            if ($retained) { $handle.DangerousRelease() }
        }
    } catch { return $null }
}

function Get-ClientCpuProgress($Client, $Baseline) {
    try {
        $sample = Get-ClientCpuSample $Client
        if ($null -eq $Baseline -or $null -eq $sample -or
            $sample.ProcessId -ne $Baseline.ProcessId -or
            $sample.Handle -ne $Baseline.Handle -or
            $sample.StartTicks -ne $Baseline.StartTicks -or
            $sample.CpuTicks -lt $Baseline.CpuTicks -or
            $sample.ElapsedTicks -lt $Baseline.ElapsedTicks) {
            return "<client CPU progress unavailable>"
        }
        $cpuMilliseconds = [long][System.Math]::Floor(
            ($sample.CpuTicks - $Baseline.CpuTicks) / 10000.0)
        $elapsedMilliseconds = $sample.ElapsedTicks - $Baseline.ElapsedTicks
        return "pid=$($sample.ProcessId) cpu_ms=$cpuMilliseconds elapsed_ms=$elapsedMilliseconds"
    } catch { return "<client CPU progress unavailable>" }
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
                $pattern = "(?m)^\[[0-9]{4}/[0-9]{2}/[0-9]{2} [0-9]{2}:[0-9]{2}:[0-9]{2}\.[0-9]{6}\] INFO[ ]+\[(?:socket_command_player|(?:SDL_)?main|Event_PollInputDevice|system_end):[0-9]{1,10}\] (?:Gameplay ready\.|First gameplay frame presented\.|Client (?:window close event received|quit event received|quit event polling completed|main loop exited|shutdown complete)\.|Client shutdown stage: (?:UI cleanup|network connection|directory workers|audio|settings and assets|image workers|image packs|asset requests|toolkit, GPU, window and SDL)\.)\r?$"
                $lines = @([regex]::Matches($text, $pattern) | Select-Object -First 32 |
                    ForEach-Object { $_.Value.TrimEnd([char]13) })
                $gpuPattern = "(?m)^\[[0-9]{4}/[0-9]{2}/[0-9]{2} [0-9]{2}:[0-9]{2}:[0-9]{2}\.[0-9]{6}\] INFO[ ]+\[(?:gpu_renderer_progress_log|gpu_renderer_create_internal):[0-9]{1,10}\] GPU progress (?:frame=[0-9]{1,20} phase=(?:before-present|after-present|present-error) map_submitted=[0-9]{1,20} map_completed=[0-9]{1,20}\.|identity backend=\[[A-Za-z0-9 ._()+,-]{1,96}\] device=\[[A-Za-z0-9 ._()+,-]{1,96}\] driver=\[[A-Za-z0-9 ._()+,-]{1,96}\] version=\[[A-Za-z0-9 ._()+,-]{1,96}\]\.)\r?$"
                $lines += @([regex]::Matches($text, $gpuPattern) | Select-Object -First 129 |
                    ForEach-Object { $_.Value.TrimEnd([char]13) })
                if ($lines.Count -eq 0) { $lines = @("<client lifecycle markers unavailable>") }
            } else {
                $lines = @("<client lifecycle log exceeds 16 Mi-character read limit>")
            }
        }
        $evidence = Join-Path (Split-Path -Parent $packagePath) "review-client-lifecycle.log"
        $identityLines = @()
        if ($null -ne $clientWarpIdentityPending) {
            try { $identityLines = @("WARP module identity: " + $clientWarpIdentityPending.Finish()) }
            catch { $identityLines = @("WARP module identity: warp_identity_unavailable") }
        }
        # Private artifact only on success; keep normal stdout lifecycle-only.
        @("revision=$Revision") + $lines + $identityLines |
            Set-Content -LiteralPath $evidence -Encoding utf8 -ErrorAction Stop
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

# Identify only a loaded System32 WARP image in the owned process. Timeout
# snapshots additionally bind the module coordinate observed in an owned stack.
# No symbols are fetched, and neither file paths nor file/memory bytes escape.
function Get-ClientWarpModuleIdentity($Client, [switch]$Initialize, [switch]$Start) {
    try {
        if (-not ("AtrinikReviewWarpIdentityProbe" -as [type])) {
            Add-Type -TypeDefinition @'
using System;
using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using System.Threading;
[assembly: DefaultDllImportSearchPaths(DllImportSearchPath.System32)]
public static class AtrinikReviewWarpIdentityProbe {
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, ExactSpelling = true)]
    private static extern uint GetFinalPathNameByHandleW(Microsoft.Win32.SafeHandles.SafeFileHandle file,
        StringBuilder path, uint capacity, uint flags);
    [DllImport("kernel32.dll")]
    private static extern uint GetProcessId(IntPtr process);
    [DllImport("kernel32.dll")]
    private static extern uint WaitForSingleObject(IntPtr handle, uint timeout);
    [DllImport("kernel32.dll")]
    private static extern bool ReadProcessMemory(IntPtr process, IntPtr address,
        byte[] bytes, UIntPtr size, out UIntPtr read);
    private const int FileLimit = 67108864, MetadataLimit = 65536;
    private static int active;
    private sealed class Identity {
        public uint Timestamp, ImageSize, Age;
        public string Guid;
        public string Key { get { return Timestamp.ToString("x8") + ":" +
            ImageSize.ToString("x8") + ":" + Guid + ":" + Age.ToString("x"); } }
    }
    private static bool Range(ulong offset, ulong count, ulong length) {
        return count <= length && offset <= length - count;
    }
    // PE32+ image headers and CodeView RSDS only; all offsets are range-checked.
    // https://learn.microsoft.com/windows/win32/debug/pe-format
    private static Identity Parse(Func<ulong, int, byte[]> read, bool mapped, ulong length) {
        Func<ulong, int, byte[]> at = (offset, count) => {
            if (count <= 0 || count > MetadataLimit || !Range(offset, (ulong)count, length))
                throw new InvalidDataException();
            var bytes = read(offset, count);
            if (bytes == null || bytes.Length != count) throw new InvalidDataException();
            return bytes;
        };
        var dos = at(0, 64);
        if (BitConverter.ToUInt16(dos, 0) != 0x5a4d) throw new InvalidDataException();
        uint peOffset = BitConverter.ToUInt32(dos, 60);
        if (peOffset > MetadataLimit) throw new InvalidDataException();
        var pe = at(peOffset, 24);
        int sections = BitConverter.ToUInt16(pe, 6), optionalSize = BitConverter.ToUInt16(pe, 20);
        if (BitConverter.ToUInt32(pe, 0) != 0x4550 || BitConverter.ToUInt16(pe, 4) != 0x8664 ||
            (BitConverter.ToUInt16(pe, 22) & 0x2000) == 0 || sections < 1 || sections > 96 ||
            optionalSize < 240 || optionalSize > 4096) throw new InvalidDataException();
        var optional = at((ulong)peOffset + 24, optionalSize);
        if (BitConverter.ToUInt16(optional, 0) != 0x20b ||
            BitConverter.ToUInt32(optional, 108) < 7) throw new InvalidDataException();
        uint imageSize = BitConverter.ToUInt32(optional, 56);
        if (imageSize == 0 || imageSize > FileLimit || (mapped && imageSize != length))
            throw new InvalidDataException();
        var table = at((ulong)peOffset + 24 + (uint)optionalSize, sections * 40);
        Func<uint, int, ulong> offsetFor = (rva, count) => {
            if (!Range(rva, (ulong)count, imageSize)) throw new InvalidDataException();
            if (mapped) return rva;
            ulong found = UInt64.MaxValue;
            for (int index = 0; index < sections; ++index) {
                int entry = index * 40;
                uint start = BitConverter.ToUInt32(table, entry + 12);
                uint size = BitConverter.ToUInt32(table, entry + 16);
                uint raw = BitConverter.ToUInt32(table, entry + 20);
                if (rva >= start && Range((ulong)rva - start, (ulong)count, size)) {
                    if (found != UInt64.MaxValue) throw new InvalidDataException();
                    found = (ulong)raw + rva - start;
                }
            }
            if (found == UInt64.MaxValue) throw new InvalidDataException();
            return found;
        };
        uint debugRva = BitConverter.ToUInt32(optional, 160);
        uint debugSize = BitConverter.ToUInt32(optional, 164);
        if (debugSize == 0 || debugSize > 4096 || debugSize % 28 != 0)
            throw new InvalidDataException();
        var debug = at(offsetFor(debugRva, (int)debugSize), (int)debugSize);
        Identity identity = null;
        for (int index = 0; index < debug.Length; index += 28) {
            if (BitConverter.ToUInt32(debug, index + 12) != 2) continue;
            uint size = BitConverter.ToUInt32(debug, index + 16);
            uint rva = BitConverter.ToUInt32(debug, index + 20);
            uint raw = BitConverter.ToUInt32(debug, index + 24);
            if (identity != null || size < 25 || size > 4096) throw new InvalidDataException();
            ulong offset = offsetFor(rva, (int)size);
            if (!mapped && offset != raw) throw new InvalidDataException();
            var cv = at(offset, (int)size);
            if (BitConverter.ToUInt32(cv, 0) != 0x53445352) throw new InvalidDataException();
            int end = Array.IndexOf(cv, (byte)0, 24), basename = 24;
            if (end < 24) throw new InvalidDataException();
            for (int pos = 24; pos < end; ++pos)
                if (cv[pos] == 47 || cv[pos] == 92) basename = pos + 1;
            // Decode only the fixed public basename, never a producer's path.
            if (end - basename != 13 ||
                !String.Equals(Encoding.ASCII.GetString(cv, basename, 13), "d3d10warp.pdb",
                               StringComparison.OrdinalIgnoreCase)) throw new InvalidDataException();
            var guid = new byte[16]; Array.Copy(cv, 4, guid, 0, 16);
            var value = new Guid(guid);
            uint age = BitConverter.ToUInt32(cv, 20);
            if (value == Guid.Empty || age == 0) throw new InvalidDataException();
            identity = new Identity { Timestamp = BitConverter.ToUInt32(pe, 8), ImageSize = imageSize,
                Guid = value.ToString("N"), Age = age };
        }
        if (identity == null) throw new InvalidDataException();
        return identity;
    }
    private static bool ModuleMatches(Process client, string path, ulong address, ulong size) {
        client.Refresh();
        var modules = client.Modules;
        if (modules.Count > 256) return false;
        foreach (ProcessModule module in modules) {
            if (!String.Equals(module.ModuleName, "d3d10warp.dll", StringComparison.OrdinalIgnoreCase)) continue;
            return String.Equals(Path.GetFullPath(module.FileName), path, StringComparison.OrdinalIgnoreCase) &&
                unchecked((ulong)module.BaseAddress.ToInt64()) == address && (ulong)module.ModuleMemorySize == size;
        }
        return false;
    }
    private static bool FileMatches(FileStream file, string expected) {
        var path = new StringBuilder(1024);
        uint length = GetFinalPathNameByHandleW(file.SafeFileHandle, path, 1024, 0);
        if (length == 0 || length >= 1024) return false;
        string actual = path.ToString();
        if (actual.StartsWith(@"\\?\", StringComparison.Ordinal)) actual = actual.Substring(4);
        return String.Equals(actual, expected, StringComparison.OrdinalIgnoreCase);
    }
    private static string Collect(Process client, IntPtr process, int processId, long started,
                                  ulong address, ulong size, Stopwatch timer) {
        string path = Path.GetFullPath(Path.Combine(Environment.SystemDirectory, "d3d10warp.dll"));
        if (IntPtr.Size != 8 || address > Int64.MaxValue || size == 0 || size > FileLimit ||
            address > (ulong)Int64.MaxValue - size ||
            !ModuleMatches(client, path, address, size) ||
            (File.GetAttributes(path) & FileAttributes.ReparsePoint) != 0) return "warp_identity_unavailable";
        using (var file = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read)) {
            if (!FileMatches(file, path) || file.Length < 64 || file.Length > FileLimit)
                return "warp_identity_unavailable";
            int budget = MetadataLimit;
            Func<ulong, int, byte[]> fileRead = (offset, count) => {
                if (timer.ElapsedMilliseconds >= 1500 || count > budget) throw new InvalidDataException();
                budget -= count;
                file.Position = checked((long)offset);
                var bytes = new byte[count];
                file.ReadExactly(bytes);
                return bytes;
            };
            Func<ulong, int, byte[]> imageRead = (offset, count) => {
                if (timer.ElapsedMilliseconds >= 1500 || count > budget || !Range(offset, (ulong)count, size))
                    throw new InvalidDataException();
                budget -= count;
                var bytes = new byte[count]; UIntPtr read;
                if (!ReadProcessMemory(process, new IntPtr((long)(address + offset)), bytes,
                    new UIntPtr((uint)count), out read) || read.ToUInt64() != (ulong)count)
                    throw new InvalidDataException();
                return bytes;
            };
            var disk = Parse(fileRead, false, (ulong)file.Length);
            var loaded = Parse(imageRead, true, size);
            if (disk.Key != loaded.Key) return "warp_identity_mismatch";
            file.Position = 0;
            string hash;
            using (var digest = IncrementalHash.CreateHash(HashAlgorithmName.SHA256)) {
                var buffer = new byte[65536];
                long remaining = file.Length;
                while (remaining != 0) {
                    if (timer.ElapsedMilliseconds >= 1500) return "warp_identity_timeout";
                    int count = file.Read(buffer, 0, (int)Math.Min(buffer.Length, remaining));
                    if (count == 0) return "warp_identity_unavailable";
                    digest.AppendData(buffer, 0, count); remaining -= count;
                }
                hash = Convert.ToHexString(digest.GetHashAndReset()).ToLowerInvariant();
            }
            if (timer.ElapsedMilliseconds >= 1500 || !FileMatches(file, path) ||
                !ModuleMatches(client, path, address, size) ||
                client.Id != processId || client.StartTime.ToUniversalTime().Ticks != started ||
                GetProcessId(process) != (uint)processId || WaitForSingleObject(process, 0) != 258 ||
                Parse(imageRead, true, size).Key != disk.Key) return "warp_identity_changed";
            return "module=d3d10warp.dll sha256=" + hash + " timestamp=" + disk.Timestamp.ToString("x8") +
                " image_size=" + disk.ImageSize.ToString("x8") + " pdb=d3d10warp.pdb guid=" +
                disk.Guid + " age=" + disk.Age.ToString("x");
        }
    }
    public sealed class Pending {
        internal Thread Observer;
        internal string Result = "warp_identity_unavailable";
        public string Finish() {
            return Observer == null || Observer.Join(2000) ? Result : "warp_identity_timeout";
        }
    }
    private static Pending Begin(Process client, int processId, long started,
                                 ulong address, ulong size, bool findLoaded) {
        var pending = new Pending();
        if (client.Id != processId || processId == Environment.ProcessId ||
            client.StartTime.ToUniversalTime().Ticks != started) {
            pending.Result = "warp_identity_wrong_process"; return pending;
        }
        var handle = client.SafeHandle;
        if (Interlocked.CompareExchange(ref active, 1, 0) != 0) {
            pending.Result = "warp_identity_busy"; return pending;
        }
        bool retained = false;
        var timer = Stopwatch.StartNew();
        var observer = new Thread(() => {
            try {
                IntPtr process = handle.DangerousGetHandle();
                if (GetProcessId(process) != (uint)processId || WaitForSingleObject(process, 0) != 258) return;
                // Never Refresh or enumerate modules on the caller's Process
                // while its main thread sends CloseMainWindow/WaitForExit.
                using (var observed = Process.GetProcessById(processId)) {
                    if (observed.StartTime.ToUniversalTime().Ticks != started) return;
                    if (findLoaded) {
                        var modules = observed.Modules;
                        if (modules.Count > 256) return;
                        foreach (ProcessModule module in modules) {
                            if (!String.Equals(module.ModuleName, "d3d10warp.dll", StringComparison.OrdinalIgnoreCase)) continue;
                            address = unchecked((ulong)module.BaseAddress.ToInt64());
                            size = (ulong)module.ModuleMemorySize;
                            break;
                        }
                        if (size == 0) { pending.Result = "warp_identity_not_loaded"; return; }
                    }
                    pending.Result = Collect(observed, process, processId, started, address, size, timer);
                }
            } catch { }
            finally { handle.DangerousRelease(); Interlocked.Exchange(ref active, 0); }
        });
        observer.IsBackground = true;
        try { handle.DangerousAddRef(ref retained); observer.Start(); }
        catch {
            if (retained) handle.DangerousRelease(); Interlocked.Exchange(ref active, 0);
            return pending;
        }
        pending.Observer = observer;
        return pending;
    }
    public static Pending StartLoaded(Process client) {
        // No wait on the application's close path. The worker owns its retained
        // handle until completion, including after caller timeout/process exit.
        return Begin(client, client.Id, client.StartTime.ToUniversalTime().Ticks, 0, 0, true);
    }
    public static string Snapshot(Process client, int processId, long started, ulong address, ulong size) {
        return Begin(client, processId, started, address, size, false).Finish();
    }
}
'@ -ErrorAction Stop
        }
        if ($Initialize) { return }
        if ($Start) { return [AtrinikReviewWarpIdentityProbe]::StartLoaded($Client) }
        $observed = [AtrinikReviewCloseStackProbe]::LastWarp
        if ($null -eq $observed) { return "warp_identity_not_observed" }
        return [AtrinikReviewWarpIdentityProbe]::Snapshot($Client, $observed.ProcessId,
            $observed.StartTicks, $observed.Address, $observed.Size)
    } catch { return "warp_identity_unavailable" }
}

# Timeout-only x64 diagnostic. Snapshot the owned thread's registers and a bounded
# stack, resume it, then unwind. Never retain raw memory, register values or paths.
function Get-ClientCloseStackSnapshot($Client, [switch]$Initialize) {
    try {
        if (-not ("AtrinikReviewCloseStackProbe" -as [type])) {
            Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text.RegularExpressions;
using System.Threading;
[assembly: DefaultDllImportSearchPaths(DllImportSearchPath.System32)]
public static class AtrinikReviewCloseStackProbe {
    [DllImport("user32.dll")]
    private static extern uint GetWindowThreadProcessId(IntPtr window, out uint processId);
    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern IntPtr OpenThread(uint access, bool inherit, uint threadId);
    [DllImport("kernel32.dll")]
    private static extern uint GetProcessIdOfThread(IntPtr thread);
    [DllImport("kernel32.dll")]
    private static extern bool GetThreadTimes(IntPtr thread, out long created,
        out long exited, out long kernel, out long user);
    [DllImport("kernel32.dll")]
    private static extern uint WaitForSingleObject(IntPtr handle, uint timeout);
    [DllImport("kernel32.dll")]
    private static extern bool CloseHandle(IntPtr handle);
    [DllImport("kernel32.dll")]
    private static extern bool IsWow64Process2(IntPtr process, out ushort machine, out ushort native);
    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern uint SuspendThread(IntPtr thread);
    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern uint ResumeThread(IntPtr thread);
    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool GetThreadContext(IntPtr thread, IntPtr context);
    [DllImport("kernel32.dll")]
    private static extern UIntPtr VirtualQueryEx(IntPtr process, IntPtr address,
        IntPtr information, UIntPtr length);
    [DllImport("kernel32.dll")]
    private static extern bool ReadProcessMemory(IntPtr process, IntPtr address,
        byte[] bytes, UIntPtr size, out UIntPtr read);
    private delegate bool ReadMemory(IntPtr process, ulong address, IntPtr buffer,
        uint size, out uint read);
    private delegate IntPtr FunctionTable(IntPtr process, ulong address);
    private delegate ulong ModuleBase(IntPtr process, ulong address);
    [DllImport("dbghelp.dll")]
    private static extern bool StackWalk64(uint machine, IntPtr process, IntPtr thread,
        IntPtr frame, IntPtr context, ReadMemory read, FunctionTable functions,
        ModuleBase modules, IntPtr translate);

    // Windows x64 ABI: winnt.h CONTEXT is 1232 bytes, aligned to 16; dbghelp.h
    // STACKFRAME64 is 264 bytes (five ADDRESS64s, fields, 112-byte KDHELP64).
    // https://learn.microsoft.com/windows/win32/api/winnt/ns-winnt-context
    // https://learn.microsoft.com/windows/win32/api/dbghelp/ns-dbghelp-stackframe64
    // https://learn.microsoft.com/windows/win32/api/dbghelp/nf-dbghelp-stackwalk64
    private const int ContextSize = 1232, FrameSize = 264, StackLimit = 65536;
    private static int active; // DbgHelp is single-threaded, including late observers.
    public sealed class WarpObservation {
        public readonly int ProcessId;
        public readonly long StartTicks;
        public readonly ulong Address, Size;
        internal WarpObservation(Process client, ulong address, ulong size) {
            ProcessId = client.Id;
            StartTicks = client.StartTime.ToUniversalTime().Ticks;
            Address = address; Size = size;
        }
    }
    private static WarpObservation lastWarp;
    public static WarpObservation LastWarp { get { return Volatile.Read(ref lastWarp); } }
    public static void ClearWarpObservation() { Volatile.Write(ref lastWarp, null); }
    // Query-only samples use the documented FILETIME (100 ns) counters:
    // https://learn.microsoft.com/windows/win32/api/processthreadsapi/nf-processthreadsapi-getthreadtimes
    // Threads born after baseline or already exited are deliberately unavailable.
    private const int ThreadLimit = 256;
    private sealed class CpuReading {
        public uint Id;
        public long Created, Cpu;
        public IntPtr Handle;
    }
    public sealed class CpuBaseline : IDisposable {
        internal readonly object Gate = new object();
        private readonly List<CpuReading> readings = new List<CpuReading>();
        private readonly Microsoft.Win32.SafeHandles.SafeProcessHandle process;
        private readonly int processId;
        private readonly long started, elapsed;
        private bool retained, disposed;
        internal CpuBaseline(Process client) {
            processId = client.Id;
            started = client.StartTime.ToUniversalTime().Ticks;
            process = client.SafeHandle;
            process.DangerousAddRef(ref retained);
            elapsed = Environment.TickCount64;
        }
        public void Dispose() {
            lock (Gate) {
                if (disposed) return;
                disposed = true;
                foreach (var reading in readings) CloseHandle(reading.Handle);
                readings.Clear();
                if (retained) { process.DangerousRelease(); retained = false; }
            }
        }
        internal bool Capture() {
            if (processId == Process.GetCurrentProcess().Id) return false;
            var timer = Stopwatch.StartNew();
            // The original process handle pins the generation while the separate
            // Process object supplies only a bounded list of thread IDs.
            using (var client = Process.GetProcessById(processId)) {
                if (client.StartTime.ToUniversalTime().Ticks != started) return false;
                var threads = client.Threads;
                try {
                    if (threads.Count > ThreadLimit) return false;
                    foreach (ProcessThread item in threads) {
                        if (timer.ElapsedMilliseconds >= 500) return false;
                        IntPtr thread = OpenThread(0x00100800, false, (uint)item.Id);
                        if (thread == IntPtr.Zero) continue;
                        bool keep = false;
                        try {
                            long created, cpu;
                            if (ReadCpu(process.DangerousGetHandle(), processId, thread,
                                        out created, out cpu)) {
                                readings.Add(new CpuReading { Id = (uint)item.Id,
                                    Created = created, Cpu = cpu, Handle = thread });
                                keep = true;
                            }
                        } finally { if (!keep) CloseHandle(thread); }
                    }
                    return readings.Count != 0;
                } finally { foreach (ProcessThread item in threads) item.Dispose(); }
            }
        }
        internal string Progress(Process client) {
            lock (Gate) {
                if (disposed || client.Id != processId || client.HasExited ||
                    client.StartTime.ToUniversalTime().Ticks != started ||
                    client.SafeHandle.DangerousGetHandle() != process.DangerousGetHandle() ||
                    Environment.TickCount64 < elapsed) return "thread_cpu_generation_unavailable";
                var current = new List<CpuReading>();
                long elapsedMilliseconds = Environment.TickCount64 - elapsed;
                var timer = Stopwatch.StartNew();
                foreach (var reading in readings) {
                    if (timer.ElapsedMilliseconds >= 500) return "thread_cpu_sample_timeout";
                    long created, cpu;
                    if (ReadCpu(process.DangerousGetHandle(), processId, reading.Handle,
                                out created, out cpu))
                        current.Add(new CpuReading { Id = reading.Id, Created = created,
                            Cpu = cpu, Handle = reading.Handle });
                }
                var ranked = RankCpu(readings, current);
                var rows = new List<string>();
                foreach (var reading in ranked) {
                    rows.Add("thread=" + reading.Id + " cpu_ms=" + reading.Cpu / 10000 +
                        "; " + SnapshotThread(client, IntPtr.Zero, reading.Id, reading.Created));
                }
                return "thread_cpu_samples=" + current.Count + " baseline=" + readings.Count +
                    " elapsed_ms=" + elapsedMilliseconds + " ranked=" + ranked.Count +
                    "; " + String.Join("; ", rows);
            }
        }
    }
    private static bool ReadCpu(IntPtr process, int processId, IntPtr thread,
                                out long created, out long cpu) {
        created = cpu = 0;
        long exited, kernel, user;
        if (WaitForSingleObject(process, 0) != 258 ||
            GetProcessIdOfThread(thread) != (uint)processId ||
            WaitForSingleObject(thread, 0) != 258 ||
            !GetThreadTimes(thread, out created, out exited, out kernel, out user) ||
            created <= 0 || kernel < 0 || user < 0 || kernel > Int64.MaxValue - user ||
            WaitForSingleObject(thread, 0) != 258) return false;
        cpu = kernel + user;
        return true;
    }
    private static List<CpuReading> RankCpu(List<CpuReading> before, List<CpuReading> after) {
        var ranked = new List<CpuReading>();
        if (before.Count > ThreadLimit || after.Count > ThreadLimit) return ranked;
        foreach (var current in after) {
            foreach (var old in before) {
                if (old.Id != current.Id || old.Created <= 0 || old.Created != current.Created ||
                    old.Handle == IntPtr.Zero || old.Handle != current.Handle ||
                    old.Cpu < 0 || current.Cpu <= old.Cpu) continue;
                ranked.Add(new CpuReading { Id = current.Id, Created = current.Created,
                    Handle = current.Handle, Cpu = current.Cpu - old.Cpu });
                break;
            }
        }
        ranked.Sort((left, right) => {
            int cpu = right.Cpu.CompareTo(left.Cpu);
            return cpu != 0 ? cpu : left.Id.CompareTo(right.Id);
        });
        if (ranked.Count > 2) ranked.RemoveRange(2, ranked.Count - 2);
        return ranked;
    }
    public static CpuBaseline CaptureCpuBaseline(Process client) {
        CpuBaseline baseline = null;
        try {
            baseline = new CpuBaseline(client);
            var gate = new object();
            bool cancelled = false, ready = false;
            var observer = new Thread(delegate() {
                bool captured = false;
                try { captured = baseline.Capture(); } catch { }
                lock (gate) {
                    if (cancelled || !captured) baseline.Dispose();
                    else ready = true;
                }
            });
            observer.IsBackground = true;
            observer.Start();
            if (observer.Join(1000)) return ready ? baseline : null;
            // The worker retains all handles until its late cleanup completes.
            lock (gate) { cancelled = true; if (ready) baseline.Dispose(); }
            return null;
        } catch { if (baseline != null) baseline.Dispose(); return null; }
    }
    public static string CpuProgress(Process client, CpuBaseline baseline) {
        try { return baseline == null ? "thread_cpu_baseline_unavailable" : baseline.Progress(client); }
        catch { return "thread_cpu_unavailable"; }
    }
    // Timeout fencing covers capture as well as unwind: the caller can remove
    // our suspend count, and cancellation prevents a late observer suspending.
    private sealed class Suspension {
        private readonly object gate = new object();
        private IntPtr thread;
        private bool held, cancelled;
        public bool Begin(IntPtr value) {
            lock (gate) {
                if (cancelled) return false;
                thread = value;
                held = SuspendThread(thread) != UInt32.MaxValue;
                return held;
            }
        }
        public bool Resume() {
            lock (gate) {
                if (!held) return true;
                if (ResumeThread(thread) == UInt32.MaxValue) return false;
                held = false;
                return true;
            }
        }
        public void Cancel() {
            lock (gate) { cancelled = true; Resume(); }
        }
        public void Close(IntPtr value) {
            lock (gate) {
                Resume();
                thread = IntPtr.Zero;
                if (value != IntPtr.Zero) CloseHandle(value);
            }
        }
    }
    private sealed class Module {
        public ulong Address, Size;
        public string Name;
        public byte[] Functions;
    }
    private static bool Contains(ulong start, ulong length, ulong address, ulong size) {
        return address >= start && size <= length && address - start <= length - size;
    }
    private static string FormatAddress(string name, ulong start, ulong length, ulong address) {
        if (!Contains(start, length, address, 1)) return "unknown_module";
        // Basenames only, even if the caller accidentally supplies a full path.
        string basename = name.Replace('\\', '/');
        basename = basename.Substring(basename.LastIndexOf('/') + 1);
        basename = Regex.Replace(basename, "[^A-Za-z0-9_.-]", "?");
        if (basename.Length > 128) basename = basename.Substring(0, 128);
        return basename + "+0x" + (address - start).ToString("x");
    }
    private static Module Find(List<Module> modules, ulong address, ulong size) {
        foreach (var module in modules)
            if (Contains(module.Address, module.Size, address, size)) return module;
        return null;
    }
    private static byte[] Read(IntPtr process, ulong address, int count) {
        var bytes = new byte[count];
        UIntPtr read;
        if (!ReadProcessMemory(process, new IntPtr(unchecked((long)address)), bytes,
            new UIntPtr((uint)count), out read) || read.ToUInt64() != (ulong)count) return null;
        return bytes;
    }
    private static string Collect(Process client, IntPtr process, IntPtr window, uint threadId,
                                  long expectedCreated, Suspension suspension) {
        IntPtr thread = IntPtr.Zero, contextAllocation = IntPtr.Zero;
        IntPtr frame = IntPtr.Zero, information = IntPtr.Zero, functionEntry = IntPtr.Zero;
        try {
            ushort machine, native;
            if (IntPtr.Size != 8 || !IsWow64Process2(process, out machine, out native) ||
                machine != 0 || native != 0x8664) return "stack_unsupported_architecture";
            var modules = new List<Module>();
            foreach (ProcessModule module in client.Modules) {
                if (modules.Count >= 256) return "stack_module_limit";
                modules.Add(new Module { Address = unchecked((ulong)module.BaseAddress.ToInt64()),
                    Size = (ulong)module.ModuleMemorySize, Name = module.ModuleName });
            }
            // GET_CONTEXT | SUSPEND_RESUME | QUERY_LIMITED_INFORMATION | SYNCHRONIZE.
            thread = OpenThread(0x0010080a, false, threadId);
            if (thread == IntPtr.Zero) return "stack_thread_unavailable";
            contextAllocation = Marshal.AllocHGlobal(ContextSize + 15);
            IntPtr context = new IntPtr((contextAllocation.ToInt64() + 15) & ~15L);
            Marshal.Copy(new byte[ContextSize], 0, context, ContextSize);
            Marshal.WriteInt32(context, 48, 0x10000b); // AMD64 CONTROL | INTEGER | FLOATING_POINT
            information = Marshal.AllocHGlobal(48);
            frame = Marshal.AllocHGlobal(FrameSize);
            Marshal.Copy(new byte[FrameSize], 0, frame, FrameSize);
            functionEntry = Marshal.AllocHGlobal(12);
            uint owner = (uint)client.Id;
            // The original Process SafeHandle remains pinned throughout: a reused
            // PID/TID must pass held-handle liveness plus HWND ownership or the
            // CPU baseline's creation time checked against this opened handle.
            if ((window != IntPtr.Zero && GetWindowThreadProcessId(window, out owner) != threadId) ||
                owner != (uint)client.Id || GetProcessIdOfThread(thread) != owner ||
                WaitForSingleObject(process, 0) != 258 ||
                WaitForSingleObject(thread, 0) != 258) return "stack_owned_thread_changed";
            long created, exited, kernel, user;
            if (expectedCreated != 0 &&
                (!GetThreadTimes(thread, out created, out exited, out kernel, out user) ||
                 created != expectedCreated)) return "stack_thread_generation_changed";
            byte[] stack;
            ulong stackAddress;
            if (!suspension.Begin(thread)) return "stack_suspend_failed_or_cancelled";
            try {
                if (!GetThreadContext(thread, context)) return "stack_context_failed";
                stackAddress = unchecked((ulong)Marshal.ReadInt64(context, 152)); // Rsp
                if (VirtualQueryEx(process, new IntPtr(unchecked((long)stackAddress)), information,
                    new UIntPtr(48)).ToUInt64() != 48) return "stack_region_unavailable";
                ulong region = unchecked((ulong)Marshal.ReadInt64(information, 0));
                ulong size = unchecked((ulong)Marshal.ReadInt64(information, 24));
                if (!Contains(region, size, stackAddress, 1)) return "stack_region_invalid";
                int count = (int)Math.Min((ulong)StackLimit, size - (stackAddress - region));
                stack = Read(process, stackAddress, count);
                if (stack == null) return "stack_capture_failed";
            } finally {
                // No DbgHelp, symbol lookup, output or callbacks while suspended.
                // Remove exactly our one suspend count, never another observer's.
                if (!suspension.Resume())
                    throw new InvalidOperationException("stack_resume_failed");
            }
            ulong instruction = unchecked((ulong)Marshal.ReadInt64(context, 248)); // Rip
            Marshal.WriteInt64(frame, 0, unchecked((long)instruction));
            Marshal.WriteInt32(frame, 12, 3); // AddrModeFlat
            Marshal.WriteInt64(frame, 32, Marshal.ReadInt64(context, 160)); // Rbp
            Marshal.WriteInt32(frame, 44, 3);
            Marshal.WriteInt64(frame, 48, unchecked((long)stackAddress));
            Marshal.WriteInt32(frame, 60, 3);
            var timer = Stopwatch.StartNew();
            int budget = 1048576;
            Func<ulong, int, byte[]> readModule = delegate(ulong address, int count) {
                if (count <= 0 || count > budget || timer.ElapsedMilliseconds >= 1500 ||
                    Find(modules, address, (ulong)count) == null) return null;
                budget -= count;
                return Read(process, address, count);
            };
            ReadMemory readMemory = delegate(IntPtr unused, ulong address, IntPtr buffer,
                                             uint count, out uint read) {
                read = 0;
                try {
                    if (count > StackLimit || timer.ElapsedMilliseconds >= 1500) return false;
                    if (Contains(stackAddress, (ulong)stack.Length, address, count)) {
                        Marshal.Copy(stack, (int)(address - stackAddress), buffer, (int)count);
                    } else {
                        var bytes = readModule(address, (int)count);
                        if (bytes == null) return false;
                        Marshal.Copy(bytes, 0, buffer, bytes.Length);
                    }
                    read = count;
                    return true;
                } catch { return false; }
            };
            ModuleBase moduleBase = delegate(IntPtr unused, ulong address) {
                var module = Find(modules, address, 1);
                return module == null ? 0 : module.Address;
            };
            FunctionTable functions = delegate(IntPtr unused, ulong address) {
                try {
                    var module = Find(modules, address, 1);
                    if (module == null) return IntPtr.Zero;
                    if (module.Functions == null) {
                        module.Functions = new byte[0]; // Attempt each module once.
                        var dos = readModule(module.Address, 64);
                        if (dos == null || BitConverter.ToUInt16(dos, 0) != 0x5a4d) return IntPtr.Zero;
                        uint offset = BitConverter.ToUInt32(dos, 60);
                        if (offset > module.Size || module.Size - offset < 264) return IntPtr.Zero;
                        var pe = readModule(module.Address + offset, 264);
                        if (pe == null || BitConverter.ToUInt32(pe, 0) != 0x4550 ||
                            BitConverter.ToUInt16(pe, 4) != 0x8664 ||
                            BitConverter.ToUInt16(pe, 24) != 0x20b ||
                            BitConverter.ToUInt32(pe, 132) < 4) return IntPtr.Zero;
                        uint rva = BitConverter.ToUInt32(pe, 160), length = BitConverter.ToUInt32(pe, 164);
                        if (length == 0 || length % 12 != 0 || length > 524288 ||
                            rva > module.Size || length > module.Size - rva) return IntPtr.Zero;
                        var table = readModule(module.Address + rva, (int)length);
                        if (table != null) module.Functions = table;
                    }
                    byte[] entries = module.Functions;
                    ulong relative = address - module.Address;
                    int low = 0, high = entries.Length / 12;
                    while (low < high) {
                        int index = low + (high - low) / 2;
                        uint begin = BitConverter.ToUInt32(entries, index * 12);
                        uint end = BitConverter.ToUInt32(entries, index * 12 + 4);
                        if (relative < begin) high = index;
                        else if (relative >= end) low = index + 1;
                        else {
                            Marshal.Copy(entries, index * 12, functionEntry, 12);
                            return functionEntry;
                        }
                    }
                } catch { }
                return IntPtr.Zero;
            };
            var rows = new List<string>();
            ulong previous = 0;
            for (int index = 0; index < 32 && timer.ElapsedMilliseconds < 1500; ++index) {
                if (instruction == 0 || instruction == previous) break;
                var module = Find(modules, instruction, 1);
                if (module == null) { rows.Add("unknown_module"); break; }
                if (String.Equals(module.Name, "d3d10warp.dll", StringComparison.OrdinalIgnoreCase))
                    Volatile.Write(ref lastWarp, new WarpObservation(client, module.Address, module.Size));
                rows.Add(FormatAddress(module.Name, module.Address, module.Size, instruction));
                previous = instruction;
                if (!StackWalk64(0x8664, process, thread, frame, context, readMemory,
                    functions, moduleBase, IntPtr.Zero)) break;
                instruction = unchecked((ulong)Marshal.ReadInt64(frame, 0));
                // StackWalk64's first result can be the initial frame.
                if (instruction == previous && index == 0 &&
                    StackWalk64(0x8664, process, thread, frame, context, readMemory,
                        functions, moduleBase, IntPtr.Zero))
                    instruction = unchecked((ulong)Marshal.ReadInt64(frame, 0));
            }
            GC.KeepAlive(readMemory); GC.KeepAlive(functions); GC.KeepAlive(moduleBase);
            Array.Clear(stack, 0, stack.Length);
            return "stack_frames=" + rows.Count + "; " + String.Join("; ", rows);
        } catch { return "stack_unavailable"; }
        finally {
            suspension.Resume();
            if (functionEntry != IntPtr.Zero) Marshal.FreeHGlobal(functionEntry);
            if (information != IntPtr.Zero) Marshal.FreeHGlobal(information);
            if (frame != IntPtr.Zero) Marshal.FreeHGlobal(frame);
            if (contextAllocation != IntPtr.Zero) {
                Marshal.Copy(new byte[ContextSize + 15], 0, contextAllocation, ContextSize + 15);
                Marshal.FreeHGlobal(contextAllocation);
            }
            suspension.Close(thread);
        }
    }
    public static string Snapshot(Process client, IntPtr window) {
        uint owner;
        uint thread = GetWindowThreadProcessId(window, out owner);
        if (window == IntPtr.Zero || thread == 0 || owner != (uint)client.Id ||
            client.Id == Process.GetCurrentProcess().Id) return "stack_owned_window_unavailable";
        return SnapshotThread(client, window, thread, 0);
    }
    private static string SnapshotThread(Process client, IntPtr window, uint thread, long created) {
        if (client.Id == Process.GetCurrentProcess().Id) return "stack_invalid_observer_pid";
        if (Interlocked.CompareExchange(ref active, 1, 0) != 0) return "stack_observer_busy";
        var handle = client.SafeHandle;
        bool retained = false;
        string result = "stack_unavailable";
        var suspension = new Suspension();
        var observer = new Thread(delegate() {
            try { result = Collect(client, handle.DangerousGetHandle(), window, thread, created, suspension); }
            finally { handle.DangerousRelease(); Interlocked.Exchange(ref active, 0); }
        });
        observer.IsBackground = true;
        try { handle.DangerousAddRef(ref retained); observer.Start(); }
        catch {
            if (retained) handle.DangerousRelease();
            Interlocked.Exchange(ref active, 0);
            return "stack_observer_unavailable";
        }
        if (observer.Join(2000)) return result;
        suspension.Cancel();
        return "stack_observer_timeout";
    }
}
'@ -ErrorAction Stop
        }
        if ($Initialize) { return }
        if ($Client.HasExited) { return "client_exited=True" }
        return [AtrinikReviewCloseStackProbe]::Snapshot($Client, $Client.MainWindowHandle)
    } catch { return "<client close stack unavailable>" }
}

function Get-ClientThreadCpuBaseline($Client) {
    try {
        $null = Get-ClientCloseStackSnapshot $Client -Initialize
        [AtrinikReviewCloseStackProbe]::ClearWarpObservation()
        return [AtrinikReviewCloseStackProbe]::CaptureCpuBaseline($Client)
    } catch { return $null }
}

function Get-ClientThreadCpuProgress($Client, $Baseline) {
    try { return [AtrinikReviewCloseStackProbe]::CpuProgress($Client, $Baseline) }
    catch { return "thread_cpu_unavailable" }
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
    # Compile before starting any owned application, never on its close path.
    $null = Get-ClientCloseStackSnapshot $null -Initialize
    $null = Get-ClientWarpModuleIdentity $null -Initialize
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
    # Child-only opt-in; the invoking shell's environment is never changed.
    $launcherStartInfo.Environment["ATRINIK_GPU_PROGRESS"] = "1"
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
    # Retain query-only handles before close. Sampling never
    # suspends a thread; stacks are collected only after the unchanged failure.
    $clientThreadCpuBaseline = Get-ClientThreadCpuBaseline $launcherClient
    $clientCpuBaseline = Get-ClientCpuSample $launcherClient
    $clientWarpIdentityPending = Get-ClientWarpModuleIdentity $launcherClient -Start
    if (-not $launcherClient.CloseMainWindow()) {
        throw "Could not request a normal close of the one-click client window"
    }
    Write-Host "Client normal close request accepted; waiting up to 30 seconds."
    if (-not $launcherClient.WaitForExit(30000)) {
        Write-Host ("Client close timeout CPU: " +
            (Get-ClientCpuProgress $launcherClient $clientCpuBaseline))
        Write-Host ("Client close timeout thread: " +
            (Get-ClientCloseTimeoutSnapshot $launcherClient))
        Write-Host ("Client close timeout stack: " +
            (Get-ClientCloseStackSnapshot $launcherClient))
        Write-Host ("Client close timeout CPU-ranked threads: " +
            (Get-ClientThreadCpuProgress $launcherClient $clientThreadCpuBaseline))
        Write-Host ("Client close timeout WARP identity: " +
            (Get-ClientWarpModuleIdentity $launcherClient))
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
    if ($null -ne $clientThreadCpuBaseline) { $clientThreadCpuBaseline.Dispose() }
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
