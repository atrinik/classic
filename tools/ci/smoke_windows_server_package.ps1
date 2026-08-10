param(
    [Parameter(Mandatory = $true)]
    [string]$Package
)

$ErrorActionPreference = "Stop"
$packagePath = (Resolve-Path -LiteralPath $Package).Path
$smokeRoot = Join-Path ([System.IO.Path]::GetTempPath()) (
    "atrinik-server-package-smoke-{0}" -f [System.Guid]::NewGuid()
)

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

    $standardInput = Join-Path $smokeRoot "stdin.txt"
    $standardOutput = Join-Path $smokeRoot "stdout.txt"
    $standardError = Join-Path $smokeRoot "stderr.txt"
    Set-Content -LiteralPath $standardInput -Value "shutdown" -Encoding ascii

    $process = Start-Process -FilePath $env:ComSpec `
        -ArgumentList @(
            "/d",
            "/c",
            "server.bat",
            "--port_mapping=off",
            "--stun_server=off"
        ) `
        -WorkingDirectory $serverRoot `
        -RedirectStandardInput $standardInput `
        -RedirectStandardOutput $standardOutput `
        -RedirectStandardError $standardError `
        -PassThru

    if (-not $process.WaitForExit(60000)) {
        Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
        throw "Packaged server did not shut down within 60 seconds"
    }

    $output = (Get-Content -LiteralPath $standardOutput -Raw) +
        (Get-Content -LiteralPath $standardError -Raw)
    if ($process.ExitCode -ne 0) {
        throw "Packaged server exited with code $($process.ExitCode):`n$output"
    }
    if ($output -notmatch "Server ready\. Waiting for connections") {
        throw "Packaged server did not reach the ready state:`n$output"
    }
    if ($output -match "Can't open regions file") {
        throw "Packaged server failed to load regions.reg:`n$output"
    }
} finally {
    if (Test-Path -LiteralPath $smokeRoot) {
        Remove-Item -LiteralPath $smokeRoot -Recurse -Force
    }
}
