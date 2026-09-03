#!/usr/bin/env python3

"""Compose a deterministic flat Windows client/server review bundle."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import re
import stat
import tempfile
import zipfile


REVISION = re.compile(r"^[0-9a-fA-F]{40}$")
FIXED_TIME = (1980, 1, 1, 0, 0, 0)
CLIENT_METADATA = {
    "README.md": "CLIENT-README.md",
    "ATTRIBUTIONS.md": "CLIENT-ATTRIBUTIONS.md",
    "LICENSE.md": "CLIENT-LICENSE.md",
    "LICENSE.txt": "CLIENT-LICENSE.txt",
    "INSTALL": "CLIENT-INSTALL",
    "INSTALL.md": "CLIENT-INSTALL.md",
}
SERVER_METADATA = {
    "README.md": "SERVER-README.md",
    "ATTRIBUTIONS.md": "SERVER-ATTRIBUTIONS.md",
    "LICENSE.md": "SERVER-LICENSE.md",
    "LICENSE.txt": "SERVER-LICENSE.txt",
}
CLIENT_PREFERRED_CONFLICTS = {"ca-bundle.crt", "libffi-8.dll"}
TEST_MARKERS = (b"--gpu-player-view", b"injected GPU conformance fault")


class BundleError(RuntimeError):
    """The input packages cannot form an unambiguous review bundle."""


def _safe_entries(archive: zipfile.ZipFile) -> tuple[str, list[zipfile.ZipInfo]]:
    files: list[zipfile.ZipInfo] = []
    roots: set[str] = set()
    seen: set[str] = set()
    for info in archive.infolist():
        name = info.filename
        path = PurePosixPath(name)
        if (
            not name
            or "\\" in name
            or path.is_absolute()
            or any(part in ("", ".", "..") for part in path.parts)
        ):
            raise BundleError(f"unsafe ZIP member: {name!r}")
        roots.add(path.parts[0])
        if info.is_dir():
            continue
        mode = info.external_attr >> 16
        if stat.S_ISLNK(mode):
            raise BundleError(f"symbolic links are not allowed: {name}")
        folded = name.casefold()
        if folded in seen:
            raise BundleError(f"duplicate or case-colliding ZIP member: {name}")
        seen.add(folded)
        files.append(info)
    if len(roots) != 1:
        raise BundleError("package must contain exactly one top-level directory")
    return next(iter(roots)), files


def _member_target(
    info: zipfile.ZipInfo,
    root: str,
    *,
    server: bool,
) -> str | None:
    parts = PurePosixPath(info.filename).parts
    relative = parts[1:]
    if server:
        if not relative or relative[0] != "server":
            return None
        relative = relative[1:]
        if relative == ("server.bat",):
            return None
        metadata = SERVER_METADATA
    else:
        metadata = CLIENT_METADATA
    if not relative:
        return None
    target = "/".join(relative)
    if len(relative) == 1:
        target = metadata.get(target, target)
    return target


def _add_package(
    payloads: dict[str, bytes],
    origins: dict[str, str],
    package: Path,
    *,
    server: bool,
) -> None:
    label = "server" if server else "client"
    with zipfile.ZipFile(package) as archive:
        root, entries = _safe_entries(archive)
        for info in entries:
            target = _member_target(info, root, server=server)
            if target is None:
                continue
            data = archive.read(info)
            folded = target.casefold()
            prior = next((name for name in payloads if name.casefold() == folded), None)
            if prior is None:
                payloads[target] = data
                origins[target] = label
                continue
            if prior != target:
                raise BundleError(f"case-colliding bundle paths: {prior} and {target}")
            if payloads[prior] == data:
                origins[prior] = "shared"
                continue
            if server and target.casefold() in CLIENT_PREFERRED_CONFLICTS:
                continue
            raise BundleError(f"different client/server payloads collide at {target}")


FINGERPRINT_HELPER = '''"""Print the SHA-256 fingerprint of a QUIC identity certificate."""
from __future__ import annotations
import hashlib
from pathlib import Path
import ssl
import sys

BEGIN = "-----BEGIN CERTIFICATE-----"
END = "-----END CERTIFICATE-----"

if len(sys.argv) != 2:
    raise SystemExit("usage: review-quic-fingerprint.py IDENTITY_FILE")
identity = Path(sys.argv[1]).read_text(encoding="ascii")
begin = identity.find(BEGIN)
end = identity.find(END, begin)
if begin < 0 or end < 0:
    raise SystemExit("QUIC certificate is missing from the identity file")
pem = identity[begin : end + len(END)]
print(hashlib.sha256(ssl.PEM_cert_to_DER_cert(pem)).hexdigest())
'''


BAT_LAUNCHER = r'''@echo off
setlocal EnableExtensions
cd /d "%~dp0"
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0run-review.ps1"
set "RESULT=%ERRORLEVEL%"
if not "%RESULT%"=="0" (
  echo.
  echo Review launch failed. See the error above.
  if not "%ATRINIK_REVIEW_NO_PAUSE%"=="1" pause
)
exit /b %RESULT%
'''


POWERSHELL_LAUNCHER = r'''$ErrorActionPreference = "Stop"
$Root = (Resolve-Path -LiteralPath $PSScriptRoot).Path
$State = Join-Path $Root "server-data"
$Sentinel = Join-Path $State ".atrinik-review-initialized"
$PasswordFile = Join-Path $State ".atrinik-review-password"
$InstallData = Join-Path $Root "install_data"
$Identity = Join-Path $State "quic-identity.pem"
$ClientData = Join-Path $Root "client-data"
$ServerLog = Join-Path $State "server.log"
$ClientLog = Join-Path $Root "client.log"
$Account = "review521"
$Character = "Review Hero"
$LaunchMutex = [System.Threading.Mutex]::new(
    $false,
    "Local\AtrinikClassicReviewUdp1731"
)
$LaunchLockHeld = $false
$Server = $null
$Client = $null
$ServerStdoutTask = $null
$ServerStderrTask = $null

function ConvertTo-ReviewArguments([string[]]$Values) {
    return (($Values | ForEach-Object { '"' + $_ + '"' }) -join " ")
}

function Stop-ReviewProcessTree([System.Diagnostics.Process]$Process, [string]$Label) {
    if ($null -eq $Process) {
        return
    }
    if (-not $Process.HasExited) {
        $Process.Kill($true)
    }
    if (-not $Process.WaitForExit(10000)) {
        throw "$Label process tree did not exit after forced containment"
    }
}

function Invoke-ReviewIcacls([string[]]$Arguments) {
    $Output = @(& icacls.exe @Arguments 2>&1)
    if ($LASTEXITCODE -ne 0) {
        throw (
            "icacls $($Arguments -join ' ') failed with exit code " +
            "$($LASTEXITCODE): $($Output -join ' ')"
        )
    }
    return $Output
}

function Protect-ReviewSecretFile([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "Secret file is missing: $Path"
    }
    $Info = Get-Item -LiteralPath $Path
    if (($Info.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw "Secret file is a reparse point: $Path"
    }
    $CurrentSid = [System.Security.Principal.WindowsIdentity]::GetCurrent().User
    $SidArgument = "*$($CurrentSid.Value)"
    [void](Invoke-ReviewIcacls -Arguments @($Path, "/reset", "/q"))
    [void](Invoke-ReviewIcacls -Arguments @($Path, "/inheritance:r", "/q"))
    [void](Invoke-ReviewIcacls -Arguments @(
        $Path,
        "/grant:r",
        "$($SidArgument):(R)",
        "/q"
    ))

    $AclFile = Join-Path $Root (
        ".atrinik-review-acl-" + [Guid]::NewGuid().ToString("N") + ".txt"
    )
    try {
        [void](Invoke-ReviewIcacls -Arguments @($Path, "/save", $AclFile, "/q"))
        $SddlLines = @(
            Get-Content -LiteralPath $AclFile -ErrorAction Stop |
                Where-Object { $_ -match "^D:" }
        )
        $ExpectedSddl = "D:P(A;;FR;;;$($CurrentSid.Value))"
        if ($SddlLines.Count -ne 1 -or $SddlLines[0] -ne $ExpectedSddl) {
            throw "Secret file ACL is not owner-only: $Path"
        }
    } finally {
        Remove-Item -LiteralPath $AclFile -Force -ErrorAction SilentlyContinue
    }
}

function Remove-ReviewSecretFile([string]$Path) {
    $Info = Get-Item -LiteralPath $Path -ErrorAction Stop
    if ($Info.PSIsContainer) {
        throw "Secret path is a directory: $Path"
    }
    if (($Info.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw "Secret file is a reparse point: $Path"
    }
    $CurrentSid = [System.Security.Principal.WindowsIdentity]::GetCurrent().User
    $SidArgument = "*$($CurrentSid.Value)"
    [void](Invoke-ReviewIcacls -Arguments @(
        $Path,
        "/grant:r",
        "$($SidArgument):(F)",
        "/q"
    ))
    Remove-Item -LiteralPath $Path -Force -ErrorAction Stop
}

function Remove-ReviewSecretFiles {
    $Candidates = [System.Collections.Generic.List[string]]::new()
    [void]$Candidates.Add($PasswordFile)
    $DataDirectories = @(
        Get-ChildItem -LiteralPath $Root -Directory -Force -ErrorAction Stop |
            Where-Object {
                $_.Name -like "server-data-stage-*" -or
                $_.Name -like "server-data-incomplete-*"
            }
    )
    foreach ($Directory in $DataDirectories) {
        [void]$Candidates.Add(
            (Join-Path $Directory.FullName ".atrinik-review-password")
        )
    }
    foreach ($Candidate in $Candidates | Select-Object -Unique) {
        if (Test-Path -LiteralPath $Candidate -PathType Leaf) {
            Remove-Item -LiteralPath $Candidate -Force -ErrorAction Stop
        }
    }
}

try {
    try {
        $LaunchLockHeld = $LaunchMutex.WaitOne(0)
    } catch [System.Threading.AbandonedMutexException] {
        $LaunchLockHeld = $true
    }
    if (-not $LaunchLockHeld) {
        throw "Another Atrinik review launcher is already starting UDP port 1731"
    }

    $ExistingEndpoint = Get-NetUDPEndpoint -LocalPort 1731 -ErrorAction SilentlyContinue |
        Select-Object -First 1
    if ($null -ne $ExistingEndpoint) {
        throw "UDP port 1731 is already owned by PID $($ExistingEndpoint.OwningProcess); stop it before launching this review"
    }

    foreach ($Required in @("atrinik.exe", "atrinik-server.exe", "python.exe", "install_data")) {
        if (-not (Test-Path -LiteralPath (Join-Path $Root $Required))) {
            throw "The complete package was not extracted: missing $Required"
        }
    }

    if (-not (Test-Path -LiteralPath $Sentinel) -or -not (Test-Path -LiteralPath $PasswordFile)) {
        if (Test-Path -LiteralPath $State) {
            $Preserved = Join-Path $Root ("server-data-incomplete-" + [Guid]::NewGuid().ToString("N"))
            Move-Item -LiteralPath $State -Destination $Preserved
            Write-Warning "Preserved incomplete server state as $Preserved"
        }
        $Stage = Join-Path $Root ("server-data-stage-" + [Guid]::NewGuid().ToString("N"))
        try {
            Copy-Item -LiteralPath $InstallData -Destination $Stage -Recurse
            New-Item -ItemType Directory -Force -Path (Join-Path $Stage "tmp") | Out-Null
            $StagePassword = Join-Path $Stage ".atrinik-review-password"
            $StageProvisionLog = Join-Path $Stage "provision.log"
            [System.IO.File]::WriteAllText(
                $StagePassword,
                [Guid]::NewGuid().ToString("N").Substring(0, 20),
                [System.Text.Encoding]::ASCII
            )
            Protect-ReviewSecretFile $StagePassword

            $ProvisionStartInfo = [System.Diagnostics.ProcessStartInfo]::new()
            $ProvisionStartInfo.FileName = Join-Path $Root "atrinik-server.exe"
            $ProvisionStartInfo.WorkingDirectory = $Root
            $ProvisionStartInfo.UseShellExecute = $false
            $ProvisionStartInfo.CreateNoWindow = $true
            $ProvisionStartInfo.Arguments = ConvertTo-ReviewArguments @(
                "--datapath=$Stage",
                "--no_console",
                "--provision_scenario",
                "--provision_account=$Account",
                "--provision_character=$Character",
                "--provision_archetype=human_male",
                "--provision_preset=basic-player",
                "--provision_password_file=$StagePassword",
                "--http_url=off",
                "--server_public=false",
                "--stun_server=off",
                "--port_mapping=off",
                "--metaserver_publish_origin=http://127.0.0.1:9",
                "--metaserver_rendezvous_origin=http://127.0.0.1:9/v1/classic",
                "--logfile=$StageProvisionLog"
            )
            $Provision = [System.Diagnostics.Process]::new()
            $Provision.StartInfo = $ProvisionStartInfo
            try {
                if (-not $Provision.Start()) {
                    throw "Could not start the isolated scenario provisioner"
                }
                if (-not $Provision.WaitForExit(60000)) {
                    Stop-ReviewProcessTree $Provision "Scenario provisioner"
                    throw "The isolated scenario provisioner did not exit within 60 seconds"
                }
                if ($Provision.ExitCode -ne 0) {
                    throw "The isolated scenario provisioner exited with code $($Provision.ExitCode)"
                }
            } finally {
                $Provision.Dispose()
            }

            New-Item -ItemType File -Path (Join-Path $Stage ".atrinik-review-initialized") | Out-Null
            Move-Item -LiteralPath $Stage -Destination $State
        } catch {
            if (Test-Path -LiteralPath $Stage) {
                $Failed = Join-Path $Root ("server-data-incomplete-" + [Guid]::NewGuid().ToString("N"))
                Move-Item -LiteralPath $Stage -Destination $Failed
            }
            throw
        }
    }

    $PasswordInfo = Get-Item -LiteralPath $PasswordFile -ErrorAction SilentlyContinue
    if ($null -eq $PasswordInfo -or $PasswordInfo.PSIsContainer -or
        (($PasswordInfo.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0)) {
        throw "The isolated review account password file is missing or unsafe"
    }
    Protect-ReviewSecretFile $PasswordFile

    $ClientMaps = Join-Path $InstallData "http\client-maps"
    if (Test-Path -LiteralPath $ClientMaps) {
        $StateHttp = Join-Path $State "http"
        New-Item -ItemType Directory -Force -Path $StateHttp | Out-Null
        $StateMaps = Join-Path $StateHttp "client-maps"
        if (Test-Path -LiteralPath $StateMaps) {
            Remove-Item -LiteralPath $StateMaps -Recurse -Force
        }
        Copy-Item -LiteralPath $ClientMaps -Destination $StateMaps -Recurse
    }
    New-Item -ItemType Directory -Force -Path (Join-Path $State "tmp") | Out-Null
    New-Item -ItemType Directory -Force -Path $ClientData | Out-Null
    if (Test-Path -LiteralPath $ClientLog) {
        Remove-Item -LiteralPath $ClientLog -Force
    }

    if (Test-Path -LiteralPath $Identity) {
        Move-Item -LiteralPath $Identity -Destination ($Identity + ".previous-" + [Guid]::NewGuid().ToString("N"))
    }

    $Started = [System.DateTime]::UtcNow
    $ServerStartInfo = [System.Diagnostics.ProcessStartInfo]::new()
    $ServerStartInfo.FileName = Join-Path $Root "atrinik-server.exe"
    $ServerStartInfo.WorkingDirectory = $Root
    $ServerStartInfo.UseShellExecute = $false
    $ServerStartInfo.CreateNoWindow = $true
    $ServerStartInfo.RedirectStandardInput = $true
    $ServerStartInfo.RedirectStandardOutput = $true
    $ServerStartInfo.RedirectStandardError = $true
    $ServerStartInfo.Arguments = ConvertTo-ReviewArguments @(
        "--datapath=$State",
        "--port_quic=1731",
        "--network_stack=ipv4=127.0.0.1",
        "--server_public=false",
        "--stun_server=off",
        "--port_mapping=off",
        "--http_url=off",
        "--metaserver_publish_origin=http://127.0.0.1:9",
        "--metaserver_rendezvous_origin=http://127.0.0.1:9/v1/classic",
        "--logfile=$ServerLog"
    )
    $Server = [System.Diagnostics.Process]::new()
    $Server.StartInfo = $ServerStartInfo
    $ServerEnvironment = @{}
    foreach ($Name in @("HTTP_PROXY", "HTTPS_PROXY", "ALL_PROXY", "NO_PROXY")) {
        $ServerEnvironment[$Name] =
            [System.Environment]::GetEnvironmentVariable($Name, "Process")
        [System.Environment]::SetEnvironmentVariable($Name, $null, "Process")
    }
    [System.Environment]::SetEnvironmentVariable(
        "NO_PROXY",
        "127.0.0.1,localhost",
        "Process"
    )
    try {
        if (-not $Server.Start()) {
            throw "Could not start the isolated review server"
        }
    } finally {
        foreach ($Name in @("HTTP_PROXY", "HTTPS_PROXY", "ALL_PROXY", "NO_PROXY")) {
            [System.Environment]::SetEnvironmentVariable(
                $Name,
                $ServerEnvironment[$Name],
                "Process"
            )
        }
    }
    $ServerStdoutTask = $Server.StandardOutput.ReadToEndAsync()
    $ServerStderrTask = $Server.StandardError.ReadToEndAsync()

    try {
        $Deadline = [System.DateTime]::UtcNow.AddSeconds(60)
        $Ready = $false
        while ([System.DateTime]::UtcNow -lt $Deadline) {
            if ($Server.HasExited) {
                throw "Server exited with code $($Server.ExitCode)"
            }
            $Endpoint = @(Get-NetUDPEndpoint -LocalPort 1731 -ErrorAction SilentlyContinue |
                Where-Object { $_.OwningProcess -eq $Server.Id } |
                Select-Object -First 1)
            $IdentityInfo = Get-Item -LiteralPath $Identity -ErrorAction SilentlyContinue
            $ServerLogText = ""
            if (Test-Path -LiteralPath $ServerLog) {
                try {
                    $ServerLogText = [System.IO.File]::ReadAllText($ServerLog)
                } catch {
                    $ServerLogText = ""
                }
            }
            if ($Endpoint.Count -eq 1 -and $Endpoint[0].LocalAddress -eq "127.0.0.1" -and
                $IdentityInfo -and $IdentityInfo.Length -gt 0 -and
                $IdentityInfo.LastWriteTimeUtc -ge $Started -and
                $ServerLogText -match "Server ready\. Waiting for connections") {
                $Ready = $true
                break
            }
            Start-Sleep -Milliseconds 250
            $Server.Refresh()
        }
        if (-not $Ready) {
            throw "Server did not reach loopback-ready state within 60 seconds"
        }

        $Fingerprint = (& (Join-Path $Root "python.exe") (Join-Path $Root "review-quic-fingerprint.py") $Identity).Trim()
        if ($LASTEXITCODE -ne 0 -or $Fingerprint -notmatch "^[0-9a-f]{64}$") {
            throw "Could not derive the server QUIC certificate fingerprint"
        }

        $ClientStartInfo = [System.Diagnostics.ProcessStartInfo]::new()
        $ClientStartInfo.FileName = Join-Path $Root "atrinik.exe"
        $ClientStartInfo.WorkingDirectory = $Root
        $ClientStartInfo.UseShellExecute = $false
        $ClientStartInfo.CreateNoWindow = $false
        $ClientStartInfo.Arguments = ConvertTo-ReviewArguments @(
            "--nometa",
            "--game_news_url=off",
            "--stun_server=off",
            "--server=127.0.0.1 1731 $Fingerprint",
            "--connect=127.0.0.1:" + $Account + "::" + $Character,
            "--connect_password_file=$PasswordFile"
        )
        $ClientStartInfoEnvironment = @{}
        foreach ($Name in @(
            "HTTP_PROXY",
            "HTTPS_PROXY",
            "ALL_PROXY",
            "NO_PROXY",
            "ATRINIK_CONFIG_DIR"
        )) {
            $ClientStartInfoEnvironment[$Name] =
                [System.Environment]::GetEnvironmentVariable($Name, "Process")
            [System.Environment]::SetEnvironmentVariable($Name, $null, "Process")
        }
        [System.Environment]::SetEnvironmentVariable(
            "NO_PROXY",
            "127.0.0.1,localhost",
            "Process"
        )
        [System.Environment]::SetEnvironmentVariable(
            "ATRINIK_CONFIG_DIR",
            $ClientData,
            "Process"
        )
        try {
            $Client = [System.Diagnostics.Process]::new()
            $Client.StartInfo = $ClientStartInfo
            if (-not $Client.Start()) {
                throw "Could not start the packaged client"
            }
        } finally {
            foreach ($Name in @(
                "HTTP_PROXY",
                "HTTPS_PROXY",
                "ALL_PROXY",
                "NO_PROXY",
                "ATRINIK_CONFIG_DIR"
            )) {
                [System.Environment]::SetEnvironmentVariable(
                    $Name,
                    $ClientStartInfoEnvironment[$Name],
                    "Process"
                )
            }
        }

        Write-Host "Server and client started. Close the client window to finish the review."
        $Client.WaitForExit()
        $Client.Refresh()
        if ($Client.ExitCode -ne 0) {
            throw "Client exited with code $($Client.ExitCode)"
        }
        if (-not (Test-Path -LiteralPath $ClientLog) -or
            ([System.IO.File]::ReadAllText($ClientLog) -notmatch "Client shutdown complete\.")) {
            throw "Client did not report a clean shutdown"
        }

        $Server.StandardInput.WriteLine("shutdown")
        $Server.StandardInput.Flush()
        $Server.StandardInput.Close()
        if (-not $Server.WaitForExit(30000)) {
            throw "Server did not exit after the graceful shutdown request"
        }
        if ($null -eq $ServerStdoutTask -or $null -eq $ServerStderrTask -or
            -not $ServerStdoutTask.Wait(10000) -or -not $ServerStderrTask.Wait(10000)) {
            throw "Server output did not close after graceful shutdown"
        }
        if ($Server.ExitCode -ne 0) {
            throw "Server exited with code $($Server.ExitCode)"
        }
        if (-not (Test-Path -LiteralPath $ServerLog) -or
            ([System.IO.File]::ReadAllText($ServerLog) -notmatch "Server shutdown complete\.")) {
            throw "Server did not report a clean shutdown"
        }
        Write-Host "Client and server exited cleanly."
    } catch {
        $Failure = $_
        $CleanupFailures = [System.Collections.Generic.List[string]]::new()
        foreach ($Entry in @(
            @{ Process = $Client; Label = "Client" },
            @{ Process = $Server; Label = "Server" }
        )) {
            if ($null -eq $Entry.Process) {
                continue
            }
            try {
                Stop-ReviewProcessTree $Entry.Process $Entry.Label
            } catch {
                $CleanupFailures.Add(
                    "$($Entry.Label): $($_.Exception.Message)"
                )
            }
        }
        if ($CleanupFailures.Count -ne 0) {
            throw (
                "$($Failure.Exception.Message); cleanup failures: " +
                ($CleanupFailures -join "; ")
            )
        }
        throw $Failure
    }
} finally {
    try {
        Remove-ReviewSecretFiles
    } finally {
        if ($null -ne $Client) {
            $Client.Dispose()
        }
        if ($null -ne $Server) {
            $Server.Dispose()
        }
        if ($LaunchLockHeld) {
            $LaunchMutex.ReleaseMutex()
        }
        $LaunchMutex.Dispose()
    }
}

'''


def compose(client: Path, server: Path, output: Path, revision: str) -> None:
    revision = revision.lower()
    if not REVISION.fullmatch(revision):
        raise BundleError("revision must be an exact 40-character hexadecimal commit")
    if output.exists():
        raise BundleError(f"refusing to overwrite existing output: {output}")

    payloads: dict[str, bytes] = {}
    origins: dict[str, str] = {}
    _add_package(payloads, origins, client, server=False)
    _add_package(payloads, origins, server, server=True)
    for required in ("atrinik.exe", "atrinik-server.exe", "python.exe", "install_data"):
        if required == "install_data":
            if not any(name.startswith("install_data/") for name in payloads):
                raise BundleError("server package is missing install_data")
        elif required not in payloads:
            raise BundleError(f"bundle is missing {required}")

    encoded_revision = revision.encode("ascii")
    for executable in ("atrinik.exe", "atrinik-server.exe"):
        if encoded_revision not in payloads[executable]:
            raise BundleError(f"{executable} does not embed exact revision {revision}")
    for marker in TEST_MARKERS:
        if marker in payloads["atrinik.exe"]:
            raise BundleError(f"production atrinik.exe contains test marker {marker.decode()}")

    payloads.update(
        {
            "run-review.bat": BAT_LAUNCHER.replace("\n", "\r\n").encode(),
            "run-review.ps1": POWERSHELL_LAUNCHER.replace("\n", "\r\n").encode(),
            "review-quic-fingerprint.py": FINGERPRINT_HELPER.encode(),
            "REVIEW-README.txt": (
                "Atrinik Classic issue #521 Windows review package\r\n"
                f"Exact revision: {revision}\r\n\r\n"
                "Extract the complete ZIP to a writable directory, then double-click "
                "run-review.bat. It provisions an isolated review account and character, "
                "starts the server on loopback UDP 1731, and launches the production client "
                "with the fresh QUIC identity. Close the client window after gameplay is ready; "
                "the launcher then requests a graceful server shutdown.\r\n"
            ).encode(),
        }
    )
    origins.update({name: "generated" for name in payloads if name.startswith("run-review")})
    origins["review-quic-fingerprint.py"] = "generated"
    origins["REVIEW-README.txt"] = "generated"
    manifest = {
        "format": 1,
        "revision": revision,
        "udp_port": 1731,
        "files": [
            {
                "path": name,
                "sha256": hashlib.sha256(data).hexdigest(),
                "size": len(data),
                "origin": origins.get(name, "generated"),
            }
            for name, data in sorted(payloads.items(), key=lambda item: item[0].casefold())
        ],
    }
    payloads["BUNDLE-MANIFEST.json"] = (
        json.dumps(manifest, indent=2, sort_keys=True) + "\n"
    ).encode()

    root = f"atrinik-classic-issue-521-windows-one-click-{revision[:7]}"
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(dir=output.parent, suffix=".zip", delete=False) as handle:
        temporary = Path(handle.name)
    try:
        with zipfile.ZipFile(temporary, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
            for name, data in sorted(payloads.items(), key=lambda item: item[0].casefold()):
                info = zipfile.ZipInfo(f"{root}/{name}", FIXED_TIME)
                info.compress_type = zipfile.ZIP_DEFLATED
                info.external_attr = (0o100644 & 0xFFFF) << 16
                archive.writestr(info, data)
        with zipfile.ZipFile(temporary) as archive:
            if archive.testzip() is not None:
                raise BundleError("composed ZIP failed CRC validation")
            _safe_entries(archive)
        temporary.replace(output)
    finally:
        temporary.unlink(missing_ok=True)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--client-package", type=Path, required=True)
    parser.add_argument("--server-package", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--revision", required=True)
    args = parser.parse_args()
    try:
        compose(args.client_package, args.server_package, args.output, args.revision)
    except (BundleError, OSError, zipfile.BadZipFile) as error:
        parser.error(str(error))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
