param(
    [Parameter(Mandatory = $true)]
    [string]$Package
)

$ErrorActionPreference = "Stop"

function Get-ProcessTreeEvidence {
    param([int]$RootProcessId)
    try {
        $processes = @(Get-CimInstance -ClassName Win32_Process -ErrorAction Stop)
    } catch {
        return "process-tree-error=$($_.Exception.Message)"
    }

    $ids = [System.Collections.Generic.HashSet[int]]::new()
    [void]$ids.Add($RootProcessId)
    $changed = $true
    while ($changed) {
        $changed = $false
        foreach ($candidate in $processes) {
            if ($ids.Contains([int]$candidate.ParentProcessId) -and $ids.Add([int]$candidate.ProcessId)) {
                $changed = $true
            }
        }
    }

    $rows = @(
        $processes |
            Where-Object { $ids.Contains([int]$_.ProcessId) } |
            Select-Object Name, ProcessId, ParentProcessId, ExecutablePath, CommandLine
    )
    if ($rows.Count -eq 0) {
        return "root-process-id=$RootProcessId (no live process rows)"
    }
    return ($rows | ConvertTo-Json -Compress)
}

function Get-PackagedServerProcesses {
    param([string]$ExecutablePath)

    try {
        return @(
            Get-CimInstance -ClassName Win32_Process -Filter "Name = 'atrinik-server.exe'" -ErrorAction Stop |
                Where-Object {
                    [string]::Equals(
                        [string]$_.ExecutablePath,
                        $ExecutablePath,
                        [System.StringComparison]::OrdinalIgnoreCase
                    )
                }
        )
    } catch {
        throw "Could not inspect packaged server processes: $($_.Exception.Message)"
    }
}

function Get-PortEvidence {
    param([int]$Port)

    try {
        return @(
            Get-NetUDPEndpoint -LocalPort $Port -ErrorAction Stop |
                Select-Object LocalAddress, LocalPort, OwningProcess
        )
    } catch {
        throw "Could not inspect packaged server UDP endpoints: $($_.Exception.Message)"
    }
}

function Get-CapturedOutput {
    param(
        [System.Collections.Generic.List[string]]$Lines,
        $RemainderTask,
        $ErrorTask
    )

    $output = $Lines -join "`n"
    if ($null -ne $RemainderTask -and $RemainderTask.IsCompleted) {
        $remainder = $RemainderTask.Result
        if ($remainder) {
            $output += "`n$remainder"
        }
    }
    if ($null -ne $ErrorTask -and $ErrorTask.IsCompleted) {
        $errorOutput = $ErrorTask.Result
        if ($errorOutput) {
            $output += "`nSTDERR:`n$errorOutput"
        }
    }
    return [string]$output
}

$packagePath = (Resolve-Path -LiteralPath $Package).Path
$smokeRoot = Join-Path ([System.IO.Path]::GetTempPath()) (
    "atrinik-server-package-smoke-{0}" -f [System.Guid]::NewGuid()
)
$process = $null
$errorTask = $null
$remainderTask = $null
$serverExecutable = $null
$stdinOpen = $false
$bodySucceeded = $false
$output = ""
$portProbe = [System.Net.Sockets.UdpClient]::new(0)
try {
    $serverPort = ([System.Net.IPEndPoint]$portProbe.Client.LocalEndPoint).Port
} finally {
    $portProbe.Dispose()
}

try {
    Expand-Archive -LiteralPath $packagePath -DestinationPath $smokeRoot
    $packageRoots = @(Get-ChildItem -LiteralPath $smokeRoot -Directory)
    if ($packageRoots.Count -ne 1) {
        throw "Expected exactly one package root, found $($packageRoots.Count)"
    }

    $serverRoot = Join-Path $packageRoots[0].FullName "server"
    $serverExecutable = Join-Path $serverRoot "atrinik-server.exe"
    $regions = Join-Path $serverRoot "maps/regions.reg"
    if (-not (Test-Path -LiteralPath $regions -PathType Leaf)) {
        throw "Packaged server is missing server/maps/regions.reg"
    }
    if (Test-Path -LiteralPath (Join-Path $packageRoots[0].FullName "maps")) {
        throw "Packaged server contains the obsolete root-level maps directory"
    }

    $startInfo = [System.Diagnostics.ProcessStartInfo]::new()
    $startInfo.FileName = $env:ComSpec
    $startInfo.RedirectStandardError = $true
    $startInfo.WorkingDirectory = $serverRoot
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    $startInfo.RedirectStandardInput = $true
    $startInfo.RedirectStandardOutput = $true
    foreach ($name in @("HTTP_PROXY", "HTTPS_PROXY", "ALL_PROXY", "NO_PROXY")) {
        [void]$startInfo.Environment.Remove($name)
    }
    $startInfo.Environment["NO_PROXY"] = "127.0.0.1,localhost"
    foreach ($argument in @(
        "/d",
        "/c",
        "call",
        "server.bat",
        "--port_quic=$serverPort",
        "--network_stack=ipv4=127.0.0.1",
        "--port_mapping=off",
        "--stun_server=off",
        "--metaserver_publish_origin=http://127.0.0.1:9",
        "--metaserver_rendezvous_origin=http://127.0.0.1:9/v1/classic"
    )) {
        $startInfo.ArgumentList.Add($argument)
    }

    $process = [System.Diagnostics.Process]::new()
    $process.StartInfo = $startInfo
    if (-not $process.Start()) {
        throw "Could not launch the packaged server"
    }
    $stdinOpen = $true
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
        $stdinState = if ($stdinOpen) { "open" } else { "closed" }
        $processTree = Get-ProcessTreeEvidence -RootProcessId $process.Id
        try {
            $process.StandardInput.Close()
        } catch {
            if (-not $process.HasExited) {
                throw
            }
        }
        $stdinOpen = $false
        if (-not $process.HasExited) {
            try {
                $process.Kill($true)
            } catch {
                if (-not $process.HasExited) {
                    throw
                }
            }
        }
        if (-not $process.WaitForExit(10000)) {
            throw "Packaged server process tree did not exit after readiness containment:`n$processTree"
        }
        $lineClosed = $lineTask.Wait(10000)
        $errorClosed = $errorTask.Wait(10000)
        if (-not $lineClosed -or -not $errorClosed) {
            throw "Packaged server readiness output did not close within 10 seconds:`nProcess tree:`n$processTree"
        }
        $lastLine = $lineTask.Result
        if ($lastLine) {
            $lines.Add($lastLine)
        }
        $errorOutput = $errorTask.Result
        $output = $lines -join "`n"
        if ($errorOutput) {
            $output += "`nSTDERR:`n$errorOutput"
        }
        throw (
            "Packaged server did not reach the ready state within 60 seconds " +
            "(stdin=$stdinState):`nSTDOUT:`n$output`n" +
            "Process tree before containment:`n$processTree"
        )
    }

    $remainderTask = $process.StandardOutput.ReadToEndAsync()
    $listenerEndpoints = @(Get-NetUDPEndpoint -LocalPort $serverPort)
    if (
        $listenerEndpoints.Count -ne 1 -or
        $listenerEndpoints[0].LocalAddress -ne "127.0.0.1"
    ) {
        $boundAddresses = ($listenerEndpoints | ForEach-Object { $_.LocalAddress }) -join ", "
        $processTree = Get-ProcessTreeEvidence -RootProcessId $process.Id
        try {
            $process.StandardInput.Close()
        } catch {
            if (-not $process.HasExited) {
                throw
            }
        }
        $stdinOpen = $false
        if (-not $process.HasExited) {
            try {
                $process.Kill($true)
            } catch {
                if (-not $process.HasExited) {
                    throw
                }
            }
        }
        if (-not $process.WaitForExit(10000)) {
            throw (
                "Packaged server process tree did not exit after listener containment:`n" +
                "$processTree"
            )
        }
        $remainderClosed = $remainderTask.Wait(10000)
        $errorClosed = $errorTask.Wait(10000)
        $failureOutput = Get-CapturedOutput -Lines $lines -RemainderTask $remainderTask -ErrorTask $errorTask
        if (-not $remainderClosed -or -not $errorClosed) {
            throw (
                "Packaged server listener output did not close within 10 seconds:`n" +
                "Captured output available before the deadline:`n$failureOutput`n" +
                "Process tree before containment:`n$processTree"
            )
        }
        throw (
            "Packaged server listener is not isolated to IPv4 loopback: $boundAddresses`n" +
            "Captured output:`n$failureOutput`n" +
            "Process tree before containment:`n$processTree"
        )
    }

    $shutdownProcessTree = $null
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
    if ($shutdownTimedOut) {
        $shutdownProcessTree = Get-ProcessTreeEvidence -RootProcessId $process.Id
    }
    $stdinState = if ($stdinOpen) { "open" } else { "closed" }
    try {
        $process.StandardInput.Close()
    } catch {
        if (-not $process.HasExited) {
            throw
        }
    }
    $stdinOpen = $false
    if ($shutdownTimedOut) {
        try {
            $process.Kill($true)
        } catch {
            if (-not $process.HasExited) {
                throw
            }
        }
        if (-not $process.WaitForExit(10000)) {
            throw "Packaged server process tree did not exit after forced containment"
        }
    }
    $remainderClosed = $remainderTask.Wait(10000)
    $errorClosed = $errorTask.Wait(10000)
    if (-not $remainderClosed -or -not $errorClosed) {
        $processTree = if ($shutdownProcessTree) {
            $shutdownProcessTree
        } else {
            Get-ProcessTreeEvidence -RootProcessId $process.Id
        }
        $partialOutput = Get-CapturedOutput -Lines $lines -RemainderTask $remainderTask -ErrorTask $errorTask
        throw (
            "Packaged server output did not close within 10 seconds:`n" +
            "Captured output available before the deadline:`n$partialOutput`n" +
            "Process tree:`n$processTree"
        )
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
        $processTree = if ($shutdownProcessTree) {
            $shutdownProcessTree
        } else {
            Get-ProcessTreeEvidence -RootProcessId $process.Id
        }
        throw (
            "Packaged server did not shut down after $shutdownAttempts " +
            "graceful attempts within 30 seconds (stdin=$stdinState):`n" +
            "STDOUT:`n$output`nProcess tree before containment:`n$processTree"
        )
    }
    if ($process.ExitCode -ne 0) {
        throw "Packaged server exited with code $($process.ExitCode):`n$output"
    }
    if ($output -match "Can't open regions file") {
        throw "Packaged server failed to load regions.reg:`n$output"
    }
    if ($output -match "Discovered a direct") {
        throw "Loopback-only packaged server advertised a direct candidate:`n$output"
    }
    $cleanupDeadline = [System.DateTime]::UtcNow.AddSeconds(10)
    $remainingServerProcesses = @()
    $remainingEndpoints = @()
    do {
        $remainingServerProcesses = @(Get-PackagedServerProcesses -ExecutablePath $serverExecutable)
        $remainingEndpoints = @(Get-PortEvidence -Port $serverPort)
        if ($remainingServerProcesses.Count -eq 0 -and $remainingEndpoints.Count -eq 0) {
            break
        }
        Start-Sleep -Milliseconds 250
    } while ([System.DateTime]::UtcNow -lt $cleanupDeadline)
    if ($remainingServerProcesses.Count -ne 0 -or $remainingEndpoints.Count -ne 0) {
        $processTree = Get-ProcessTreeEvidence -RootProcessId $process.Id
        $remainingProcessText = if ($remainingServerProcesses.Count -gt 0) {
            $remainingServerProcesses |
                Select-Object Name, ProcessId, ParentProcessId, ExecutablePath, CommandLine |
                ConvertTo-Json -Compress
        } else {
            "none"
        }
        $remainingEndpointText = if ($remainingEndpoints.Count -gt 0) {
            $remainingEndpoints | ConvertTo-Json -Compress
        } else {
            "none"
        }
        throw (
            "Packaged server cleanup did not complete within 10 seconds:`n" +
            "Remaining processes:`n$remainingProcessText`n" +
            "Remaining UDP endpoints:`n$remainingEndpointText`n" +
            "Captured output:`n$output`n" +
            "Process tree:`n$processTree"
        )
    }
    $bodySucceeded = $true
} finally {
    $cleanupFailures = [System.Collections.Generic.List[string]]::new()
    if ($null -ne $process) {
        try {
            $process.StandardInput.Close()
        } catch {
            Write-Warning "Could not close packaged server input: $_"
        }
        $stdinOpen = $false
        try {
            if (-not $process.HasExited) {
                $process.Kill($true)
            }
            if (-not $process.WaitForExit(10000)) {
                $cleanupFailures.Add("Packaged server process tree did not exit within 10 seconds")
            }
        } catch {
            Write-Warning "Could not stop packaged server process tree: $_"
            $cleanupFailures.Add("Could not stop packaged server process tree")
        } finally {
            try {
                $process.Dispose()
            } catch {
                Write-Warning "Could not dispose packaged server process: $_"
                $cleanupFailures.Add("Could not dispose packaged server process")
            }
        }
    }
    if ($null -ne $serverExecutable) {
        try {
            $remainingServerProcesses = @(Get-PackagedServerProcesses -ExecutablePath $serverExecutable)
            $remainingEndpoints = @(Get-PortEvidence -Port $serverPort)
            if ($remainingServerProcesses.Count -ne 0 -or $remainingEndpoints.Count -ne 0) {
                $cleanupFailures.Add(
                    "Packaged server cleanup left " +
                    "$($remainingServerProcesses.Count) process(es) and " +
                    "$($remainingEndpoints.Count) UDP endpoint(s)"
                )
            }
        } catch {
            Write-Warning "Could not verify packaged server cleanup: $_"
            $cleanupFailures.Add("Could not verify packaged server cleanup")
        }
    }
    if (Test-Path -LiteralPath $smokeRoot) {
        for ($attempt = 1; $attempt -le 5; $attempt++) {
            try {
                Remove-Item -LiteralPath $smokeRoot -Recurse -Force
                break
            } catch {
                if ($attempt -eq 5) {
                    Write-Warning "Could not remove smoke directory: $_"
                } else {
                    Start-Sleep -Seconds 1
                }
            }
        }
        if (Test-Path -LiteralPath $smokeRoot) {
            $cleanupFailures.Add("Could not remove packaged server smoke directory")
        }
    }
    if ($bodySucceeded -and $cleanupFailures.Count -ne 0) {
        throw (
            ($cleanupFailures -join "; ") + "`nCaptured output:`n$output"
        )
    }
    if (-not $bodySucceeded -and $cleanupFailures.Count -ne 0) {
        Write-Warning ("Packaged server smoke cleanup findings: " + ($cleanupFailures -join "; "))
    }
}
