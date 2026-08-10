param(
    [Parameter(Mandatory = $true)]
    [string]$Package
)

$ErrorActionPreference = "Stop"
$packagePath = (Resolve-Path -LiteralPath $Package).Path
$smokeRoot = Join-Path ([System.IO.Path]::GetTempPath()) (
    "atrinik-server-package-smoke-{0}" -f [System.Guid]::NewGuid()
)
$process = $null
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
    $regions = Join-Path $serverRoot "maps/regions.reg"
    if (-not (Test-Path -LiteralPath $regions -PathType Leaf)) {
        throw "Packaged server is missing server/maps/regions.reg"
    }
    if (Test-Path -LiteralPath (Join-Path $packageRoots[0].FullName "maps")) {
        throw "Packaged server contains the obsolete root-level maps directory"
    }

    $startInfo = [System.Diagnostics.ProcessStartInfo]::new()
    $startInfo.FileName = $env:ComSpec
    $startInfo.WorkingDirectory = $serverRoot
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    $startInfo.RedirectStandardInput = $true
    $startInfo.RedirectStandardOutput = $true
    foreach ($argument in @(
        "/d",
        "/c",
        "server.bat",
        "--port_quic=$serverPort",
        "--network_stack=ipv4=127.0.0.1",
        "--port_mapping=off",
        "--stun_server=off",
        "--metaserver_publish_origin=http://127.0.0.1:9",
        "--metaserver_rendezvous_origin=http://127.0.0.1:9/v1/classic",
        "2>&1"
    )) {
        $startInfo.ArgumentList.Add($argument)
    }

    $process = [System.Diagnostics.Process]::new()
    $process.StartInfo = $startInfo
    if (-not $process.Start()) {
        throw "Could not launch the packaged server"
    }

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
        $output = $lines -join "`n"
        throw "Packaged server did not reach the ready state within 60 seconds:`n$output"
    }

    $listenerEndpoints = @(Get-NetUDPEndpoint -LocalPort $serverPort)
    if (
        $listenerEndpoints.Count -ne 1 -or
        $listenerEndpoints[0].LocalAddress -ne "127.0.0.1"
    ) {
        $boundAddresses = ($listenerEndpoints | ForEach-Object { $_.LocalAddress }) -join ", "
        throw "Packaged server listener is not isolated to IPv4 loopback: $boundAddresses"
    }

    $process.StandardInput.WriteLine("shutdown")
    $process.StandardInput.Flush()
    $process.StandardInput.Close()
    $remainderTask = $process.StandardOutput.ReadToEndAsync()
    if (-not $process.WaitForExit(30000)) {
        $output = $lines -join "`n"
        throw "Packaged server did not shut down within 30 seconds:`n$output"
    }
    if (-not $remainderTask.Wait(10000)) {
        $output = $lines -join "`n"
        throw "Packaged server output did not close within 10 seconds:`n$output"
    }

    $remainder = $remainderTask.Result
    if ($remainder) {
        $lines.Add($remainder)
        Write-Host $remainder
    }
    $output = $lines -join "`n"
    if ($process.ExitCode -ne 0) {
        throw "Packaged server exited with code $($process.ExitCode):`n$output"
    }
    if ($output -match "Can't open regions file") {
        throw "Packaged server failed to load regions.reg:`n$output"
    }
} finally {
    if ($null -ne $process) {
        try {
            $process.StandardInput.Close()
        } catch {
            Write-Warning "Could not close packaged server input: $_"
        }
        try {
            if (-not $process.HasExited) {
                $process.Kill($true)
            }
            if (-not $process.WaitForExit(10000)) {
                Write-Warning "Packaged server process tree did not exit during cleanup"
            }
        } catch {
            Write-Warning "Could not stop packaged server process tree: $_"
        } finally {
            $process.Dispose()
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
    }
}
