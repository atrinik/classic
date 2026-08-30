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
$launcherServer = $null
$launcherClient = $null
$bodySucceeded = $false
$portProbe = [System.Net.Sockets.UdpClient]::new(0)
try {
    $serverPort = ([System.Net.IPEndPoint]$portProbe.Client.LocalEndPoint).Port
} finally {
    $portProbe.Dispose()
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

    $launcherStartInfo = [System.Diagnostics.ProcessStartInfo]::new()
    $launcherStartInfo.FileName = $env:ComSpec
    $launcherStartInfo.WorkingDirectory = $reviewRoot
    $launcherStartInfo.UseShellExecute = $false
    $launcherStartInfo.CreateNoWindow = $true
    $launcherStartInfo.ArgumentList.Add("/d")
    $launcherStartInfo.ArgumentList.Add("/c")
    $launcherStartInfo.ArgumentList.Add("call")
    $launcherStartInfo.ArgumentList.Add($launchers[0].FullName)
    $launcherProcess = [System.Diagnostics.Process]::new()
    $launcherProcess.StartInfo = $launcherStartInfo
    if (-not $launcherProcess.Start()) {
        throw "Could not execute the user-facing run-review.bat launcher"
    }
    if (-not $launcherProcess.WaitForExit(60000)) {
        $launcherProcess.Kill($true)
        throw "run-review.bat did not return after launching the review processes"
    }
    if ($launcherProcess.ExitCode -ne 0) {
        throw "run-review.bat exited with code $($launcherProcess.ExitCode)"
    }
    $launcherDeadline = [System.DateTime]::UtcNow.AddSeconds(60)
    $serverExecutable = Join-Path $reviewRoot "atrinik-server.exe"
    $clientExecutable = Join-Path $reviewRoot "atrinik.exe"
    while ([System.DateTime]::UtcNow -lt $launcherDeadline) {
        $launcherServers = @(Get-Process -Name "atrinik-server" -ErrorAction SilentlyContinue |
            Where-Object { $_.Path -eq $serverExecutable })
        $launcherClients = @(Get-Process -Name "atrinik" -ErrorAction SilentlyContinue |
            Where-Object { $_.Path -eq $clientExecutable })
        if ($launcherServers.Count -eq 1 -and $launcherClients.Count -eq 1) {
            $launcherServer = $launcherServers[0]
            $launcherClient = $launcherClients[0]
            break
        }
        Start-Sleep -Milliseconds 250
    }
    if ($null -eq $launcherServer -or $null -eq $launcherClient) {
        throw "One-click launcher did not start exactly one packaged server and client"
    }
    $launcherEndpoints = @(Get-NetUDPEndpoint -LocalPort 1731 | Where-Object {
        $_.OwningProcess -eq $launcherServer.Id
    })
    if ($launcherEndpoints.Count -ne 1) {
        throw "One-click launcher server does not own UDP port 1731"
    }
    Start-Sleep -Seconds 5
    $launcherServer.Refresh()
    $launcherClient.Refresh()
    if ($launcherServer.HasExited -or $launcherClient.HasExited) {
        throw "One-click launcher server or client exited during the startup smoke interval"
    }
    Stop-Process -Id $launcherClient.Id -Force
    Stop-Process -Id $launcherServer.Id -Force
    [void]$launcherClient.WaitForExit(10000)
    [void]$launcherServer.WaitForExit(10000)
    $bodySucceeded = $true
} finally {
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
